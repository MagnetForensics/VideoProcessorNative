﻿#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include "libavutil/display.h"
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "libvx.h"

#ifdef __cplusplus
#pragma error
#endif

#ifdef DEBUG
#define dprintf(...) { printf("%s:%d %-30s ", __FILE__, __LINE__, __func__); printf(__VA_ARGS__); }
#else
#define dprintf(...)
#endif

#define FRAME_QUEUE_SIZE 32
#define FRAME_BUFFER_PADDING 4096
#define LOG_TRACE_BUFSIZE 4096

static bool initialized = false;
static int retry_count = 100;
static vx_log_callback log_cb = NULL;

struct vx_rectangle
{
	int x;
	int y;
	int width;
	int height;
};

struct vx_audio_info
{
	double peak_level;
	double rms_level;
	double rms_peak;
};

struct vx_scene_info
{
	double difference;
	double scene_score;
	bool new_scene;
};

struct vx_frame
{
	int width;
	int height;
	vx_pix_fmt pix_fmt;
	int audio_sample_count;

	vx_audio_info audio_info;
	vx_scene_info scene_info;

	uint8_t** audio_buffer;
	void* buffer;
};

struct vx_frame_info
{
	int width;
	int height;
	double timestamp;
	vx_frame_flag flags;
};

struct vx_audio_params
{
	int channels;
	vx_sample_fmt sample_format;
	int sample_rate;
};

struct vx_video_options
{
	vx_audio_params audio_params;
	bool autorotate;
	vx_rectangle crop_area;
	vx_hwaccel_flag hw_criteria;
	float scene_threshold;
};

struct vx_video
{
	AVFormatContext* fmt_ctx;
	AVCodecContext* video_codec_ctx;
	AVCodecContext* audio_codec_ctx;
	AVBufferRef* hw_device_ctx;
	SwrContext* swr_ctx;

	enum AVPixelFormat hw_pix_fmt;

	AVFilterGraph* filter_pipeline;
	AVFilterGraph* filter_pipeline_audio;

	int video_stream;
	int audio_stream;

	long frame_count;
	int frame_queue_count;
	AVFrame* frame_queue[FRAME_QUEUE_SIZE];

	vx_video_options options;

	double ts_last;
	int64_t ts_offset;
};

static vx_log_level av_to_vx_log_level(const int level)
{
	// See: lavu_log_constants
	switch (level) {
	case AV_LOG_QUIET:
		return VX_LOG_NONE;

	case AV_LOG_PANIC:
	case AV_LOG_FATAL:
		return VX_LOG_FATAL;

	case AV_LOG_ERROR:
		return VX_LOG_ERROR;

	case AV_LOG_INFO:
	case AV_LOG_VERBOSE:
		return VX_LOG_INFO;

	case AV_LOG_DEBUG:
		return VX_LOG_DEBUG;

	default:
		return VX_LOG_NONE;
	}
}

static void vx_log_set_cb(vx_log_callback cb)
{
	log_cb = cb;
}

static void vx_log_cb(const void* avcl, int level, const char* fmt, void* vl)
{
	// This level of detail won't be needed and results in a huge number of callbacks
	if (!log_cb || level == AV_LOG_TRACE) {
		return;
	}

	if (avcl) {
		// Unused parameter
	}

	const char message[LOG_TRACE_BUFSIZE] = { NULL };

	if (vsprintf_s(&message, LOG_TRACE_BUFSIZE, fmt, vl) > 0) {
		log_cb(message, av_to_vx_log_level(level));
	}
}

static enum AVPixelFormat vx_to_av_pix_fmt(vx_pix_fmt fmt)
{
	enum AVPixelFormat formats[] = { AV_PIX_FMT_RGB24, AV_PIX_FMT_GRAY8, AV_PIX_FMT_BGRA };
	return formats[fmt];
}

static enum AVPixelFormat vx_get_hw_pixel_format(const AVBufferRef* hw_device_ctx)
{
	enum AVPixelFormat format = AV_PIX_FMT_NONE;

	const AVHWFramesConstraints* frame_constraints = av_hwdevice_get_hwframe_constraints(hw_device_ctx, NULL);

	if (frame_constraints) {
		// Take the first valid format, in the same way that av_hwframe_transfer_data will do
		// The list of format is always terminated by AV_PIX_FMT_NONE,
		// or the list will be NULL if the information is not known
		format = frame_constraints->valid_sw_formats[0];

		av_hwframe_constraints_free(&frame_constraints);
	}

	return format;
}

static bool vx_is_packet_error(int result)
{
	return result != 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF;
}

static bool vx_rectangle_is_initialized(vx_rectangle rect)
{
	return (rect.x + rect.y + rect.width + rect.height) > 0;
}

static int vx_enqueue_qsort_fn(AVFrame* a, AVFrame* b)
{
	const AVFrame* frame_a = *(AVFrame**)a;
	const AVFrame* frame_b = *(AVFrame**)b;

	return (int)(frame_b->best_effort_timestamp - frame_a->best_effort_timestamp);
}

static void vx_enqueue(vx_video* video, AVFrame* frame)
{
	video->frame_queue[video->frame_queue_count++] = frame;
	qsort(video->frame_queue, video->frame_queue_count, sizeof(AVFrame*), &vx_enqueue_qsort_fn);
}

static AVFrame* vx_dequeue(vx_video* video)
{
	return video->frame_queue[--video->frame_queue_count];
}

static AVFrame* vx_get_first_queue_item(const vx_video* video)
{
	return video->frame_queue[video->frame_queue_count - 1];
}

static vx_error vx_get_rotation_transform(const AVStream* stream, char** out_transform, char** out_transform_args)
{
	vx_error result = VX_ERR_UNKNOWN;

	uint8_t* displaymatrix = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, NULL);

	if (displaymatrix) {
		double theta = av_display_rotation_get((int32_t*)displaymatrix);

		if (theta < -135 || theta > 135) {
			*out_transform = "vflip,hflip";
			*out_transform_args = NULL;
		}
		else if (theta < -45) {
			*out_transform = "transpose";
			*out_transform_args = "dir=clock";
		}
		else if (theta > 45) {
			*out_transform = "transpose";
			*out_transform_args = "dir=cclock";
		}

		result = VX_ERR_SUCCESS;
	}
	else {
		result = VX_ERR_STREAM_INFO;
	}

	return result;
}

static vx_error vx_insert_filter(AVFilterContext** last_filter, int* pad_index, const char* filter_name, const char* filter_label, const char* args)
{
	vx_error result = VX_ERR_INIT_FILTER;
	AVFilterGraph* graph = (*last_filter)->graph;
	AVFilterContext* filter_ctx;
	if (!filter_label) {
		filter_label = filter_name;
	}

	if (avfilter_graph_create_filter(&filter_ctx, avfilter_get_by_name(filter_name), filter_label, args, NULL, graph) < 0)
		return result;

	if (avfilter_link(*last_filter, *pad_index, filter_ctx, 0) < 0)
		return result;

	*last_filter = filter_ctx;
	*pad_index = 0;

	return VX_ERR_SUCCESS;
}

static vx_error vx_initialize_audio_filter(AVFilterContext** last_filter, const int* pad_index)
{
	int result = VX_ERR_UNKNOWN;
	char transform_args[] = "metadata=1:reset=1:measure_overall=Peak_level+RMS_level+RMS_peak";

	if ((result = vx_insert_filter(last_filter, pad_index, "astats", NULL, transform_args)) != VX_ERR_SUCCESS)
		return result;

	return VX_ERR_SUCCESS;
}

static vx_error vx_initialize_crop_filter(AVFilterContext** last_filter, const int* pad_index, const vx_rectangle crop_area)
{
	int result = VX_ERR_UNKNOWN;
	char transform_args[100];

	snprintf(transform_args, sizeof(transform_args),
		"w=%d:h=%d:x=%d:y=%d:exact=1",
		crop_area.width, crop_area.height, crop_area.x, crop_area.y);

	if (transform_args) {
		if ((result = vx_insert_filter(last_filter, pad_index, "crop", NULL, transform_args)) != VX_ERR_SUCCESS)
			return result;
	}

	return VX_ERR_SUCCESS;
}

static vx_error vx_initialize_scene_filter(AVFilterContext** last_filter, const int* pad_index, const float threshold)
{
	int result = VX_ERR_UNKNOWN;
	char transform_args[100];

	snprintf(transform_args, sizeof(transform_args), "threshold=%f", threshold);

	if (transform_args) {
		if ((result = vx_insert_filter(last_filter, pad_index, "scdet", NULL, transform_args)) != VX_ERR_SUCCESS)
			return result;
	}

	return VX_ERR_SUCCESS;
}

static vx_error vx_initialize_rotation_filter(const AVStream* stream, AVFilterContext** last_filter, const int* pad_index)
{
	int result = VX_ERR_UNKNOWN;
	char* transform = NULL;
	char* transform_args = NULL;
	const char* token = NULL;

	result = vx_get_rotation_transform(stream, &transform, &transform_args);
	if (result != VX_ERR_SUCCESS && result != VX_ERR_STREAM_INFO) {
		return result;
	}

	if (transform) {
		// Parse each filter separately if several are returned
		size_t transform_size = strlen(transform) + 1;
		char* src = malloc(transform_size);
		if (!src)
			return VX_ERR_ALLOCATE;
		memcpy(src, transform, transform_size);
		token = strtok(src, ",");

		while (token) {
			if (vx_insert_filter(last_filter, pad_index, token, NULL, transform_args) != VX_ERR_SUCCESS)
				break;
			token = strtok(NULL, ",");
		}
		free(src);
	}
	
	return VX_ERR_SUCCESS;
}

static vx_error vx_init_filter_pipeline(vx_video* video)
{
	vx_error result = VX_ERR_INIT_FILTER;
	const AVStream* video_stream = video->fmt_ctx->streams[video->video_stream];
	AVFilterContext* filter_source;
	AVFilterContext* last_filter;
	int pad_index = 0;
	char args[512];

	video->filter_pipeline = avfilter_graph_alloc();

	if (!video->filter_pipeline) {
		result = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	// Set the correct pixel format, we need to find the format that a hardware frame will
	// be converted to after transferring to system memory, but before converting via scaling
	enum AVPixelFormat format = video->video_codec_ctx->pix_fmt;
	if (video->hw_device_ctx)
	{
		format = vx_get_hw_pixel_format(video->hw_device_ctx);
		if (!format) {
			av_log(NULL, AV_LOG_ERROR, "Cannot find compatible pixel format\n");
			goto cleanup;
		}
	}

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		video->video_codec_ctx->width, video->video_codec_ctx->height, format,
		video_stream->time_base.num, video_stream->time_base.den,
		video->video_codec_ctx->sample_aspect_ratio.num, video->video_codec_ctx->sample_aspect_ratio.den);

	// Create the filter pipeline video source
	if (avfilter_graph_create_filter(&filter_source, avfilter_get_by_name("buffer"), "in", args, NULL, video->filter_pipeline) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto cleanup;
	}

	// Create and link up the filter nodes
	last_filter = filter_source;
	if (video->options.autorotate)
		if ((result = vx_initialize_rotation_filter(video_stream, &last_filter, &pad_index)) != VX_ERR_SUCCESS)
			goto cleanup;

	if (vx_rectangle_is_initialized(video->options.crop_area))
		if ((result = vx_initialize_crop_filter(&last_filter, &pad_index, video->options.crop_area)) != VX_ERR_SUCCESS)
			goto cleanup;

	if (video->options.scene_threshold >= 0)
		if ((result = vx_initialize_scene_filter(&last_filter, &pad_index, video->options.scene_threshold)) != VX_ERR_SUCCESS)
			goto cleanup;

	if ((result = vx_insert_filter(&last_filter, &pad_index, "buffersink", "out", NULL)) != VX_ERR_SUCCESS)
		goto cleanup;

	// Finally, construct the filter graph using all the linked nodes
	if (avfilter_graph_config(video->filter_pipeline, NULL) < 0) {
		goto cleanup;
	}

cleanup:
	return result;
}

static vx_error vx_init_audio_filter_pipeline(vx_video* video)
{
	vx_error result = VX_ERR_INIT_FILTER;
	const AVStream* audio_stream = video->fmt_ctx->streams[video->audio_stream];
	AVFilterContext* filter_source;
	AVFilterContext* last_filter;
	int pad_index = 0;
	char args[512];

	video->filter_pipeline_audio = avfilter_graph_alloc();

	if (!video->filter_pipeline_audio) {
		result = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	if (video->audio_codec_ctx->channel_layout == 0)
		video->audio_codec_ctx->channel_layout = av_get_default_channel_layout(video->audio_codec_ctx->channels);


	char layout[100];
	av_get_channel_layout_string(layout, sizeof(layout), video->audio_codec_ctx->channels, video->audio_codec_ctx->channel_layout);

	snprintf(args, sizeof(args),
		"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s:channels=%d",
		audio_stream->time_base.num, audio_stream->time_base.den, video->audio_codec_ctx->sample_rate,
		av_get_sample_fmt_name(video->audio_codec_ctx->sample_fmt), layout, video->audio_codec_ctx->channels);

	// Create the filter pipeline audio source
	if (avfilter_graph_create_filter(&filter_source, avfilter_get_by_name("abuffer"), "in", args, NULL, video->filter_pipeline_audio) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto cleanup;
	}

	// Create and link up the filter nodes
	last_filter = filter_source;
	if ((result = vx_initialize_audio_filter(&last_filter, &pad_index)) != VX_ERR_SUCCESS)
		goto cleanup;

	if ((result = vx_insert_filter(&last_filter, &pad_index, "abuffersink", "out", NULL)) != VX_ERR_SUCCESS)
		goto cleanup;

	// Finally, construct the filter graph using all the linked nodes
	if (avfilter_graph_config(video->filter_pipeline_audio, NULL) < 0) {
		goto cleanup;
	}

cleanup:
	return result;
}

static bool use_hw(const vx_video* video, const AVCodec* codec)
{
	if (video->options.hw_criteria & VX_HW_ACCEL_ALL)
		return true;

	int height = vx_get_height(video);

	if (video->options.hw_criteria & VX_HW_ACCEL_720 && height >= 720)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_1080 && height >= 1080)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_1440 && height >= 1440)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_2160 && height >= 2160)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_HEVC && codec->id == AV_CODEC_ID_HEVC)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_H264 && codec->id == AV_CODEC_ID_H264)
		return true;

	return false;
}

static const AVCodecHWConfig* get_hw_config(const AVCodec* codec)
{
	enum AVHWDeviceType type_priority[] = {
		AV_HWDEVICE_TYPE_VDPAU,
		AV_HWDEVICE_TYPE_D3D11VA,
		AV_HWDEVICE_TYPE_CUDA,
		AV_HWDEVICE_TYPE_VAAPI,
		AV_HWDEVICE_TYPE_DXVA2,
		AV_HWDEVICE_TYPE_QSV,
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
		AV_HWDEVICE_TYPE_DRM,
		AV_HWDEVICE_TYPE_OPENCL,
		AV_HWDEVICE_TYPE_MEDIACODEC,
	};

	for (int j = 0; j < sizeof(type_priority) / sizeof(enum AVHWDeviceType); j++)
	{
		enum AVHWDeviceType target_type = type_priority[j];

		for (int i = 0;; i++)
		{
			const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);

			if (config != NULL && config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == target_type)
			{
				dprintf("found hardware config: %s\n", av_hwdevice_get_type_name(config->device_type));
				return config;
			}

			if (config == NULL)
				break;
		}
	}

	return NULL;
}

static int hw_decoder_init(vx_video* me, AVCodecContext* ctx, const enum AVHWDeviceType type)
{
	int err = 0;

	if ((err = av_hwdevice_ctx_create(&me->hw_device_ctx, type, NULL, NULL, 0)) < 0)
	{
		dprintf("Failed to create specified HW device.\n");
		return err;
	}

	ctx->hw_device_ctx = av_buffer_ref(me->hw_device_ctx);

	// Decoder does not assign sufficient pool size for mpeg2
	if (ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO && ctx->extra_hw_frames < 16) {
		ctx->extra_hw_frames = 16;
	}

	return err;
}

static bool find_stream_and_open_codec(vx_video* me, enum AVMediaType type,
	int* out_stream, AVCodecContext** out_codec_ctx, vx_error* out_error)
{
	AVCodec* codec;
	AVCodecContext* codec_ctx;

	*out_stream = av_find_best_stream(me->fmt_ctx, type, -1, -1, &codec, 0);

	if (*out_stream < 0)
	{
		if (*out_stream == AVERROR_STREAM_NOT_FOUND)
			*out_error = type == AVMEDIA_TYPE_AUDIO ? VX_ERR_NO_AUDIO : VX_ERR_VIDEO_STREAM;

		if (*out_stream == AVERROR_DECODER_NOT_FOUND)
			*out_error = VX_ERR_FIND_CODEC;

		return false;
	}

	// Get a pointer to the codec context for the video stream
	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		*out_error = VX_ERR_ALLOCATE;
		return false;
	}

	avcodec_parameters_to_context(codec_ctx, me->fmt_ctx->streams[*out_stream]->codecpar);
	*out_codec_ctx = codec_ctx;

	// Find and enable any hardware acceleration support
	const AVCodecHWConfig* hw_config = use_hw(me, codec) ? get_hw_config(codec) : NULL;

	if (hw_config != NULL)
	{
		hw_decoder_init(me, *out_codec_ctx, hw_config->device_type);
		me->hw_pix_fmt = hw_config->pix_fmt;
	}

	// Open codec
	if (avcodec_open2(*out_codec_ctx, codec, NULL) < 0)
	{
		*out_error = VX_ERR_OPEN_CODEC;
		return false;
	}

	return true;
}

static int64_t vx_get_channel_layout(const AVCodecContext* ctx)
{
	return ctx->channel_layout != 0
		? ctx->channel_layout
		: av_get_default_channel_layout(ctx->channels);
}

vx_error vx_set_audio_params(vx_video* me, int sample_rate, int channels, vx_sample_fmt format)
{
	vx_error err = VX_ERR_UNKNOWN;
	const AVCodecContext* ctx = me->audio_codec_ctx;

	if (!ctx)
		return VX_ERR_NO_AUDIO;

	if (me->swr_ctx)
		swr_free(&me->swr_ctx);

	enum AVSampleFormat avfmt = format == VX_SAMPLE_FMT_FLT
		? AV_SAMPLE_FMT_FLT
		: AV_SAMPLE_FMT_S16;
	int64_t src_channel_layout = vx_get_channel_layout(ctx);

	me->swr_ctx = swr_alloc_set_opts(
		NULL,
		av_get_default_channel_layout(channels),
		avfmt,
		sample_rate,
		src_channel_layout,
		ctx->sample_fmt,
		ctx->sample_rate,
		0,
		NULL);

	if (!me->swr_ctx) {
		err = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	swr_init(me->swr_ctx);

	return VX_ERR_SUCCESS;

cleanup:

	if (me->swr_ctx)
		swr_free(&me->swr_ctx);

	return err;
}

vx_error vx_open(vx_video** video, const char* filename, const vx_video_options options)
{
	if (!initialized) {
		// Log messages with this level, or lower, will be send to stderror
		av_log_set_level(AV_LOG_FATAL);
		if (&vx_log_cb) {
			// Redirect all libav error messages to a callback instead of stderror
			av_log_set_callback(&vx_log_cb);
		}
		initialized = true;
	}

	vx_video* me = calloc(1, sizeof(vx_video));

	if (!me)
		return VX_ERR_ALLOCATE;

	me->hw_pix_fmt = AV_PIX_FMT_NONE;
	me->ts_last = 0;
	me->ts_offset = AV_NOPTS_VALUE;
	me->options = options;

	vx_error error = VX_ERR_UNKNOWN;

	// Open stream
	int open_result = avformat_open_input(&me->fmt_ctx, filename, NULL, NULL);
	if (open_result != 0) {
		if (open_result == AVERROR(ENOENT)) {
			error = VX_ERR_FILE_NOT_FOUND;
		}
		else {
			error = VX_ERR_OPEN_FILE;
		}
		goto cleanup;
	}

	// Get stream information
	if (avformat_find_stream_info(me->fmt_ctx, NULL) < 0) {
		error = VX_ERR_STREAM_INFO;
		goto cleanup;
	}

	// Find video and audio streams and open respective codecs
	if (!find_stream_and_open_codec(me, AVMEDIA_TYPE_VIDEO, &me->video_stream, &me->video_codec_ctx, &error)) {
		goto cleanup;
	}

	if (!find_stream_and_open_codec(me, AVMEDIA_TYPE_AUDIO,&me->audio_stream, &me->audio_codec_ctx, &error)) {
		if (me->video_codec_ctx->pix_fmt == AV_PIX_FMT_NONE) {
			// The file doesn't contain video or audio information, but don't exit
			// as we can still get some useful information out by opening the file
			av_log(NULL, AV_LOG_ERROR, "The file does not contain any frame images or audio data\n");
		}
		else {
			av_log(NULL, AV_LOG_INFO, "The file does not contain any audio data\n");
		}
	}

	if (me->video_codec_ctx->pix_fmt != AV_PIX_FMT_NONE) {
		if ((error = vx_init_filter_pipeline(me)) != VX_ERR_SUCCESS)
			goto cleanup;
	}

	if (me->audio_codec_ctx) {
		vx_audio_params params = me->options.audio_params;
		if (me->options.audio_params.channels > 0) {
			if ((error = vx_set_audio_params(me, params.sample_rate, params.channels, params.sample_format)) != VX_ERR_SUCCESS) {
				goto cleanup;
			}
		}

		if ((error = vx_init_audio_filter_pipeline(me)) != VX_ERR_SUCCESS)
			goto cleanup;
	}

	*video = me;
	return VX_ERR_SUCCESS;

cleanup:
	vx_close(me);
	return error;
}

void vx_close(vx_video* video)
{
	if (!video) {
		return;
	}

	if (video->filter_pipeline)
		avfilter_graph_free(&video->filter_pipeline);

	if (video->swr_ctx)
		swr_free(&video->swr_ctx);

	if (video->fmt_ctx)
		avformat_free_context(video->fmt_ctx);

	for (int i = 0; i < video->frame_queue_count; i++) {
		av_frame_unref(video->frame_queue[i]);
		av_frame_free(&video->frame_queue[i]);
	}

	free(video);
}

static bool vx_read_frame(AVFormatContext* fmt_ctx, AVPacket* packet, int stream)
{
	// Try to read a frame, if it can't be read, skip ahead a bit and try again
	int64_t last_fp = avio_tell(fmt_ctx->pb);

	for (int i = 0; i < 1024; i++) {
		int ret = av_read_frame(fmt_ctx, packet);

		// Success
		if (ret == 0)
			return true;

		// End of file, no need to retry
		if (ret == AVERROR_EOF || avio_feof(fmt_ctx->pb))
			return false;

		// Other error, might be a damaged stream, seek forward a couple bytes and try again
		if ((i % 10) == 0) {
			int64_t fp = avio_tell(fmt_ctx->pb);

			if (fp <= last_fp)
				fp = last_fp + 100 * (int64_t)i;

			dprintf("retry: @%" PRId64 "\n", fp);
			avformat_seek_file(fmt_ctx, stream, fp + 100, fp + 512, fp + 1024 * 1024, AVSEEK_FLAG_BYTE | AVSEEK_FLAG_ANY);

			last_fp = fp;
		}
	}

	return false;
}

vx_error vx_count_frames(vx_video* me, int* out_num_frames)
{
	int num_frames = 0;

	AVPacket* packet = av_packet_alloc();
	if (!packet) {
		return VX_ERR_ALLOCATE;
	}

	while (true) {
		if (!vx_read_frame(me->fmt_ctx, packet, me->video_stream)) {
			break;
		}

		if (packet->stream_index == me->video_stream) {
			num_frames++;
		}

		av_packet_unref(packet);
	}

	av_packet_unref(packet);
	av_packet_free(&packet);

	*out_num_frames = num_frames;

	return VX_ERR_SUCCESS;
}

static bool vx_video_is_rotated(vx_video video)
{
	char* transform = NULL;
	char* transform_args = NULL;

	return video.options.autorotate
		&& vx_get_rotation_transform(video.fmt_ctx->streams[video.video_stream], &transform, &transform_args) == VX_ERR_SUCCESS
		&& transform == "transpose"
		&& (transform_args == "dir=clock" || transform_args == "dir=cclock");
}

/// <summary>
/// Attempt to calculate frame dimensions based on cropping and rotation options,
/// before the actual frame dimensions are known (i.e. after filtering)
/// </summary>
vx_error vx_get_adjusted_frame_dimensions(const vx_video* video, int* width, int* height)
{
	if (vx_rectangle_is_initialized(video->options.crop_area)) {
		*width = video->options.crop_area.width;
		*height = video->options.crop_area.height;
	}
	else if (vx_video_is_rotated(*video)) {
		int temp = *width;
		*width = *height;
		*height = temp;
	}

	return VX_ERR_SUCCESS;
}

/// <summary>
/// The original video width, before cropping but after auto rotation (if enabled)
/// </summary>
int vx_get_width(const vx_video* video)
{
	return vx_video_is_rotated(*video) ? video->video_codec_ctx->height : video->video_codec_ctx->width;
}

/// <summary>
/// The original video height, before cropping but after auto rotation (if enabled)
/// </summary>
int vx_get_height(const vx_video* video)
{
	return vx_video_is_rotated(*video) ? video->video_codec_ctx->width : video->video_codec_ctx->height;
}

/// <summary>
/// The video width, after cropping and auto rotation (if enabled)
/// </summary>
int vx_get_adjusted_width(const vx_video* video)
{
	int width = video->video_codec_ctx->width;
	int height = video->video_codec_ctx->height;
	vx_get_adjusted_frame_dimensions(video, &width, &height);

	return width;
}

/// <summary>
/// The video height, after cropping and auto rotation (if enabled)
/// </summary>
int vx_get_adjusted_height(const vx_video* video)
{
	int width = video->video_codec_ctx->width;
	int height = video->video_codec_ctx->height;
	vx_get_adjusted_frame_dimensions(video, &width, &height);

	return height;
}

long long vx_get_file_position(const vx_video* video)
{
	return video->fmt_ctx->pb->pos;
}

long long vx_get_file_size(const vx_video* video)
{
	return avio_size(video->fmt_ctx->pb);
}

int vx_get_audio_sample_rate(const vx_video* me)
{
	if (!me->audio_codec_ctx)
		return 0;

	return me->audio_codec_ctx->sample_rate;
}

int vx_get_audio_present(const vx_video* me)
{
	return me->audio_codec_ctx ? 1 : 0;
}

int vx_get_audio_channels(const vx_video* me)
{
	if (!me->audio_codec_ctx)
		return 0;

	return me->audio_codec_ctx->channels;
}

static double vx_timestamp_to_seconds_internal(const vx_video* video, const int stream_type, const long long ts)
{
	return (double)ts * av_q2d(video->fmt_ctx->streams[stream_type]->time_base);
}

double vx_timestamp_to_seconds(const vx_video* video, const long long ts)
{
	return vx_timestamp_to_seconds_internal(video, video->video_stream, ts);
}

double vx_estimate_timestamp(vx_video* video, const int stream_type, const int64_t pts)
{
	if (video->ts_offset == AV_NOPTS_VALUE && stream_type == video->video_stream)
		video->ts_offset = pts;

	double ts_estimated = 0.0;
	double ts_seconds = vx_timestamp_to_seconds_internal(video, stream_type, pts - video->ts_offset);
	double ts_delta = ts_seconds - video->ts_last;

	// Not all codecs supply a timestamp, or they supply values that don't progress nicely
	// So sometimes we need to estimate based on FPS
	if (pts == AV_NOPTS_VALUE || ts_delta <= 0 || ts_delta >= 2) {
		// Initial timestamp should be zero
		if (video->ts_last < 0 || video->frame_count == 0) {
			video->ts_last = 0;
		}
		else if (stream_type == video->video_stream) {
			float fps = 0;
			if (vx_get_frame_rate(video, &fps) == VX_ERR_SUCCESS) {
				double delta = 1.0 / fps;

				ts_estimated += delta;
			}
		}
	}
	else if (pts > 0) {
		// Use the decoded timestamp
		ts_estimated = ts_delta;
	}
	else {
		video->ts_last = ts_seconds;
	}

	return stream_type == video->video_stream
		? video->ts_last += ts_estimated
		: video->ts_last + ts_estimated;
}

static vx_error vx_frame_init_audio_buffer(const vx_video* video, vx_frame* frame)
{
	vx_error err = VX_ERR_SUCCESS;
	int line_size;

	int ret = av_samples_alloc_array_and_samples(
		&frame->audio_buffer,
		&line_size,
		video->options.audio_params.channels,
		video->options.audio_params.sample_rate * 4,
		(int)video->options.audio_params.sample_format,
		0);

	if (ret < 0) {
		err = VX_ERR_ALLOCATE;
	}

	return err;
}

vx_error vx_frame_init_buffer(vx_frame* frame)
{
	vx_error result = VX_ERR_ALLOCATE;
	int av_pixfmt = vx_to_av_pix_fmt(frame->pix_fmt);

	// Includes some padding as a workaround for a bug in swscale (?) where it overreads the buffer
	int buffer_size = av_image_get_buffer_size(av_pixfmt, frame->width, frame->height, 1) + FRAME_BUFFER_PADDING;

	if (buffer_size <= 0)
		return result;

	frame->buffer = av_mallocz(buffer_size);

	if (!frame->buffer)
		return result;

	return VX_ERR_SUCCESS;
}

vx_frame* vx_frame_create(const vx_video* video, int width, int height, vx_pix_fmt pix_fmt)
{
	vx_frame* frame = calloc(1, sizeof(vx_frame));

	if (!frame)
		goto error;

	frame->width = width;
	frame->height = height;
	frame->pix_fmt = pix_fmt;

	if (video->audio_codec_ctx && video->options.audio_params.channels > 0) {
		if (vx_frame_init_audio_buffer(video, frame) != VX_ERR_SUCCESS)
			goto error;
	}

	if (vx_frame_init_buffer(frame) != VX_ERR_SUCCESS)
		goto error;

	return frame;

error:
	if (frame)
		free(frame);

	return NULL;
}

void vx_frame_destroy(vx_frame* me)
{
	av_free(me->buffer);

	if (me->audio_buffer) {
		av_freep(&me->audio_buffer[0]);
		av_freep(&me->audio_buffer);
	}

	free(me);
}

void* vx_frame_get_video_buffer(vx_frame* frame, int* out_buffer_size)
{
	int av_pixfmt = vx_to_av_pix_fmt(frame->pix_fmt);
	*out_buffer_size = av_image_get_buffer_size(av_pixfmt, frame->width, frame->height, 1) + FRAME_BUFFER_PADDING;

	return frame->buffer;
}

void* vx_frame_get_audio_buffer(const vx_frame* frame, int* out_sample_count)
{
	*out_sample_count = frame->audio_sample_count;

	return frame->audio_buffer[0];
}

vx_audio_info vx_frame_get_audio_info(const vx_frame* frame)
{
	return frame->audio_info;
}

vx_scene_info vx_frame_get_scene_info(const vx_frame* frame)
{
	return frame->scene_info;
}

static vx_error vx_decode_frame(vx_video* me, static AVFrame* out_frame_buffer[50], int* out_frames_count, int* out_stream_idx)
{
	vx_error ret = VX_ERR_UNKNOWN;
	AVCodecContext* codec_ctx = NULL;
	AVPacket* packet = NULL;
	AVFrame* frame = NULL;
	int frame_count = 0;
	*out_frames_count = 0;
	*out_stream_idx = -1;

	packet = av_packet_alloc();
	if (!packet) {
		ret = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	// Get a packet, which will usually be a single video frame, or several complete audio frames
	vx_read_frame(me->fmt_ctx, packet, me->video_stream);

	// Only attempt to decode packets from the streams that have been selected
	// and only decode audio if it has been enabled
	if (packet->data && packet->stream_index != me->video_stream && packet->stream_index != me->audio_stream) {
		ret = VX_ERR_SUCCESS;
		goto cleanup;
	}

	// If the packet is empty then the end of the video has been reached. Howevert the decoder may still hold a couple
	// of cached frames and needs to be flushed. This is done by sending an empty packet
	if (!packet->data) {
		if (me->video_codec_ctx) {
			*out_stream_idx = me->video_stream;
			codec_ctx = me->video_codec_ctx;
		}
		else if (me->audio_codec_ctx) {
			*out_stream_idx = me->audio_stream;
			codec_ctx = me->audio_codec_ctx;
		}
		else {
			ret = VX_ERR_EOF;
			goto cleanup;
		}
	}
	else {
		*out_stream_idx = packet->stream_index;
		codec_ctx = packet->stream_index == me->video_stream
			? me->video_codec_ctx
			: me->audio_codec_ctx;
	}

	// The decoder may still hold a couple of cached frames, so even if the end of the file has been
	// reached and no packet is returned, it still needs to be sent in order to flush the decoder
	int result = avcodec_send_packet(codec_ctx, packet);
	if (result == AVERROR_EOF) {
		ret = VX_ERR_EOF;
		goto cleanup;
	}
	if (vx_is_packet_error(result)) {
		ret = VX_ERR_DECODE_VIDEO;
		goto cleanup;
	}

	// Don't process audio frames unless audio decoding is enabled
	if (packet->stream_index == me->audio_stream && !me->swr_ctx) {
		ret = VX_ERR_SUCCESS;
		goto cleanup;
	}

	// Store all frames returned by the decoder
	while (result >= 0) {
		frame = av_frame_alloc();
		result = avcodec_receive_frame(codec_ctx, frame);

		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			break;
		}
		else if (result < 0) {
			ret = VX_ERR_DECODE_VIDEO;
			goto cleanup;
		}
		else {
			if (frame_count < 50) {
				out_frame_buffer[frame_count++] = frame;
			}
			else {
				// Dump the frame and the rest of the packet data to prevent buffer overrun
				break;
			}
		}
	}

	*out_frames_count = frame_count;
	ret = VX_ERR_SUCCESS;

cleanup:
	if (frame) {
		av_frame_unref(frame);
		av_frame_free(&frame);
	}

	if (packet && packet->data)
		av_packet_unref(packet);
	av_packet_free(&packet);

	return ret;
}

static double vx_frame_metadata_as_double(const AVFrame* av_frame, const char* key)
{
	const AVDictionaryEntry* entry = av_dict_get(av_frame->metadata, key, NULL, AV_DICT_MATCH_CASE);

	return entry ? atof(entry->value) : 0;
}

static vx_error vx_frame_properties_from_metadata(vx_frame* frame, const AVFrame* av_frame)
{
	vx_audio_info audio_info = { 0 };
	vx_scene_info scene_info = { 0, 0, false };

	// TODO: 0 is the maximum db level, so a different default should be returned
	audio_info.peak_level = vx_frame_metadata_as_double(av_frame, "lavfi.astats.Overall.Peak_level");
	audio_info.rms_level = vx_frame_metadata_as_double(av_frame, "lavfi.astats.Overall.RMS_level");
	audio_info.rms_peak = vx_frame_metadata_as_double(av_frame, "lavfi.astats.Overall.RMS_peak");

	scene_info.difference = vx_frame_metadata_as_double(av_frame, "lavfi.scd.mafd");
	scene_info.scene_score = vx_frame_metadata_as_double(av_frame, "lavfi.scd.score");
	scene_info.new_scene = av_dict_get(av_frame->metadata, "lavfi.scd.time", NULL, AV_DICT_MATCH_CASE) != NULL;

	frame->audio_info = audio_info;
	frame->scene_info = scene_info;

	return VX_ERR_SUCCESS;
}

static vx_error vx_filter_frame(const vx_video* video, AVFrame* av_frame)
{
	vx_error result = VX_ERR_UNKNOWN;
	int ret = 0;

	if (video->filter_pipeline && video->filter_pipeline->nb_filters > 1) {
		const AVFilterContext* filter_source = avfilter_graph_get_filter(video->filter_pipeline, "in");
		const AVFilterContext* filter_sink = avfilter_graph_get_filter(video->filter_pipeline, "out");

		if (!(filter_source && filter_sink)) {
			result = VX_ERR_INIT_FILTER;
			goto cleanup;
		}

		if (av_buffersrc_add_frame_flags(filter_source, av_frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
			av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
			goto cleanup;
		}

		// The frame is being reused so we have to tidy it up
		av_frame_unref(av_frame);

		while (ret == 0) {
			ret = av_buffersink_get_frame(filter_sink, av_frame);

			if (vx_is_packet_error(ret)) {
				av_log(NULL, AV_LOG_ERROR, "Error retrieving frames from the filtergraph\n");
				goto cleanup;
			}
		}
	}

	result = VX_ERR_SUCCESS;

cleanup:
	return result;
}

static vx_error vx_filter_audio_frame(const vx_video* video, AVFrame* av_frame)
{
	vx_error result = VX_ERR_UNKNOWN;
	int ret = 0;

	if (video->filter_pipeline_audio && video->filter_pipeline_audio->nb_filters > 1) {
		const AVFilterContext* filter_source = avfilter_graph_get_filter(video->filter_pipeline_audio, "in");
		const AVFilterContext* filter_sink = avfilter_graph_get_filter(video->filter_pipeline_audio, "out");

		if (!(filter_source && filter_sink)) {
			result = VX_ERR_INIT_FILTER;
			goto cleanup;
		}

		if (av_buffersrc_add_frame_flags(filter_source, av_frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
			av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
			goto cleanup;
		}

		// The frame is being reused so we have to tidy it up, otherwise it will leak memory
		av_frame_unref(av_frame);

		while (ret == 0) {
			ret = av_buffersink_get_frame(filter_sink, av_frame);

			if (vx_is_packet_error(ret)) {
				av_log(NULL, AV_LOG_ERROR, "Error retrieving frames from the filtergraph\n");
				goto cleanup;
			}
		}
	}

	result = VX_ERR_SUCCESS;

cleanup:
	return result;
}

static vx_error vx_scale_frame(const AVFrame* av_frame, vx_frame* frame)
{
	vx_error ret = VX_ERR_UNKNOWN;
	int av_pixfmt = vx_to_av_pix_fmt(frame->pix_fmt);

	struct SwsContext* sws_ctx = sws_getContext(
		av_frame->width, av_frame->height, av_frame->format,
		frame->width, frame->height, av_pixfmt,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);

	if (!sws_ctx) {
		ret = VX_ERR_SCALING;
		goto cleanup;
	}

	assert(av_frame->data);

	int fmtBytesPerPixel[3] = { 3, 1, 4 };

	uint8_t* pixels[3] = { frame->buffer, 0, 0 };
	int pitch[3] = { fmtBytesPerPixel[frame->pix_fmt] * frame->width, 0, 0 };

	sws_scale(sws_ctx, (const uint8_t* const*)av_frame->data, av_frame->linesize, 0, av_frame->height, pixels, pitch);

	sws_freeContext(sws_ctx);

	return VX_ERR_SUCCESS;

cleanup:
	return ret;
}

static vx_error vx_frame_process_audio(vx_video* video, AVFrame* av_frame, vx_frame* frame)
{
	if (!video->audio_codec_ctx || !video->swr_ctx) {
		return VX_ERR_DECODE_AUDIO;
	}

	vx_audio_params params = video->options.audio_params;
	int64_t channel_layout = vx_get_channel_layout(video->audio_codec_ctx);

	int dst_sample_count = (int)av_rescale_rnd(av_frame->nb_samples, params.sample_rate, video->audio_codec_ctx->sample_rate, AV_ROUND_UP);

	if (params.channels != video->audio_codec_ctx->channels
		|| channel_layout != video->audio_codec_ctx->channel_layout
		|| params.sample_rate != video->audio_codec_ctx->sample_rate
		|| (int)params.sample_format != video->audio_codec_ctx->sample_fmt)
	{
		dprintf("audio format changed\n");
		dprintf("channels:       %d -> %d\n", params.channels, video->audio_codec_ctx->channels);
		dprintf("channel layout: %08"PRIx64" -> %08"PRIx64"\n", channel_layout, video->audio_codec_ctx->channel_layout);
		dprintf("sample rate:    %d -> %d\n", params.sample_rate, video->audio_codec_ctx->sample_rate);
		dprintf("sample format:  %d -> %d\n", params.sample_format, video->audio_codec_ctx->sample_fmt);

		// Reinitialize swr_ctx if the audio codec magically changed parameters
		vx_set_audio_params(video, params.sample_rate, params.channels, params.sample_format);
	}

	int swrret = swr_convert(video->swr_ctx, frame->audio_buffer, dst_sample_count, (const uint8_t**)av_frame->data, av_frame->nb_samples);

	if (swrret < 0) {
		return VX_ERR_RESAMPLE_AUDIO;
	}

	frame->audio_sample_count = dst_sample_count;

	return VX_ERR_SUCCESS;
}

vx_error vx_queue_frames(vx_video* me)
{
	vx_error ret = VX_ERR_SUCCESS;
	static AVFrame* frame_buffer[50] = { NULL };
	int frame_idx = 0;
	int frame_count = 0;

	// Only top up the queue in chunks instead of for every single frame
	if (me->frame_queue_count > FRAME_QUEUE_SIZE / 4) {
		return VX_ERR_SUCCESS;
	}

	// Leave some room in the frame queue to buffer the return from the decoder
	while (ret == VX_ERR_SUCCESS && me->frame_queue_count < FRAME_QUEUE_SIZE / 2) {
		int stream_idx = -1;

		ret = vx_decode_frame(me, &frame_buffer, &frame_count, &stream_idx);

		if (ret != VX_ERR_SUCCESS && frame_count <= 0)
			goto cleanup;

		// The decoder usually only returns a single video frame, but there may be several audio frames
		for (int i = 0; i < frame_count; i++) {
			if (me->frame_queue_count < FRAME_QUEUE_SIZE) {
				vx_enqueue(me, frame_buffer[i]);
			}
			else {
				ret = VX_ERR_UNKNOWN;
				frame_idx = i;
				goto cleanup;
			}
		}
	}

	return ret;

cleanup:
	for (int i = frame_idx; i < frame_count; i++) {
		AVFrame* frame = frame_buffer[i];
		if (frame) {
			av_frame_unref(frame);
			av_frame_free(&frame);
		}
	}

	return ret;
}

vx_error vx_frame_step_internal(vx_video* me, vx_frame_info* frame_info)
{
	vx_error ret = VX_ERR_UNKNOWN;
	AVFrame* frame = NULL;

	// Free the first item in the queue (if any)
	if (me->frame_queue_count > 0) {
		frame = vx_dequeue(me);

		if (!frame) {
			return VX_ERR_UNKNOWN;
		}

		av_frame_unref(frame);
		av_frame_free(&frame);
	}

	// (Re)fill the frame queue
	ret = vx_queue_frames(me);

	if (me->frame_queue_count > 0) {
		frame = vx_get_first_queue_item(me);

		int stream_type = frame->pict_type != AV_PICTURE_TYPE_NONE
			? me->video_stream
			: me->audio_stream;

		// Have to return the calculated frame dimensions here. The dimensions are
		// needed before they are actually known, i.e. after filtering
		frame_info->width = frame->width;
		frame_info->height = frame->height;
		vx_get_adjusted_frame_dimensions(me, &frame_info->width, &frame_info->height);
		// TODO: Handle duplicate audio timestamps
		frame_info->timestamp = vx_estimate_timestamp(me, stream_type, frame->best_effort_timestamp);
		frame_info->flags = frame->pict_type != AV_PICTURE_TYPE_NONE ? VX_FF_HAS_IMAGE : 0;
		frame_info->flags |= frame->nb_samples > 0 ? VX_FF_HAS_AUDIO : 0;
		frame_info->flags |= frame->pict_type == AV_PICTURE_TYPE_I ? VX_FF_KEYFRAME : 0;
		if (frame->pkt_pos != -1)
			frame_info->flags |= frame->pkt_pos < 0 ? VX_FF_BYTE_POS_GUESSED : 0;
		frame_info->flags |= frame->pts > 0 ? VX_FF_HAS_PTS : 0;

		// Override errors that may be returned when queuing frames until
		// the frame queue is processed
		ret = VX_ERR_SUCCESS;
	}

	return ret;
}

vx_error vx_frame_step(vx_video* me, vx_frame_info* out_frame_info)
{
	vx_error first_error = VX_ERR_SUCCESS;

	for (int i = 0; i < retry_count; i++)
	{
		vx_error e = vx_frame_step_internal(me, out_frame_info);

		if (!(e == VX_ERR_UNKNOWN || e == VX_ERR_VIDEO_STREAM || e == VX_ERR_DECODE_VIDEO ||
			e == VX_ERR_DECODE_AUDIO || e == VX_ERR_NO_AUDIO || e == VX_ERR_RESAMPLE_AUDIO))
		{
			me->frame_count++;

			return e;
		}

		first_error = first_error != VX_ERR_SUCCESS ? first_error : e;
	}

	return first_error;
}

vx_error vx_frame_transfer_data(const vx_video* video, vx_frame* frame)
{
	vx_error ret = VX_ERR_UNKNOWN;
	AVFrame* av_frame = NULL;

	if (video->frame_queue_count <= 0)
		return VX_ERR_EOF;

	// Get the first item from the queue, but do not dequeue
	av_frame = vx_get_first_queue_item(video);
	if (!av_frame)
		goto cleanup;

	// Copy the frame from GPU memory if it has been hardware decoded
	bool hw_decoded = av_frame->pict_type != AV_PICTURE_TYPE_NONE && av_frame->hw_frames_ctx;
	if (hw_decoded)
	{
		AVFrame* sw_frame = av_frame_alloc();

		if (!sw_frame)
			goto cleanup;

		if (av_hwframe_transfer_data(sw_frame, av_frame, 0) < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Error transferring frame data to system memory\n");
			goto cleanup;
		}

		av_frame = sw_frame;
	}

	// Run the frame through the filter pipeline, if any
	if (av_frame->pict_type != AV_PICTURE_TYPE_NONE) {
		if (vx_filter_frame(video, av_frame) != VX_ERR_SUCCESS) {
			goto cleanup;
		}
	}
	else if (av_frame->nb_samples > 0) {
		if (vx_filter_audio_frame(video, av_frame) != VX_ERR_SUCCESS) {
			goto cleanup;
		}
	}

	// Frame properties that may have been updated after filtering
	if (vx_frame_properties_from_metadata(frame, av_frame) != VX_ERR_SUCCESS) {
		goto cleanup;
	}

	if (av_frame->pict_type != AV_PICTURE_TYPE_NONE) {
		// The frame dimensions may have changed since it was initialized
		if (frame->width != av_frame->width || frame->height != av_frame->height) {
			av_free(frame->buffer);

			frame->width = av_frame->width;
			frame->height = av_frame->height;

			if (vx_frame_init_buffer(frame) != VX_ERR_SUCCESS)
				goto cleanup;
		}

		// Fill the buffer
		if (vx_scale_frame(av_frame, frame) != VX_ERR_SUCCESS) {
			goto cleanup;
		}

		if (hw_decoded) {
			av_frame_unref(av_frame);
			av_frame_free(&av_frame);
		}
	}
	else {
		// Fill the audio buffer
		if ((ret = vx_frame_process_audio(video, av_frame, frame)) != VX_ERR_SUCCESS) {
			goto cleanup;
		}
	}

	return VX_ERR_SUCCESS;

cleanup:
	if (av_frame) {
		av_frame_unref(av_frame);
		av_frame_free(&av_frame);
	}

	return ret;
}

vx_error vx_get_frame_rate(const vx_video* video, float* out_fps)
{
	AVRational rate = video->fmt_ctx->streams[video->video_stream]->avg_frame_rate;

	if (rate.num == 0 || rate.den == 0)
		return VX_ERR_FRAME_RATE;

	*out_fps = (float)av_q2d(rate);
	return VX_ERR_SUCCESS;
}

vx_error vx_get_duration(const vx_video* video, float* out_duration)
{
	*out_duration = (float)video->fmt_ctx->duration / (float)AV_TIME_BASE;
	return VX_ERR_SUCCESS;
}

bool vx_get_hw_context_present(const vx_video* video)
{
	return video->hw_device_ctx != NULL;
}

vx_error vx_get_pixel_aspect_ratio(const vx_video* video, float* out_par)
{
	AVRational par = video->video_codec_ctx->sample_aspect_ratio;
	if (par.num == 0 && par.den == 1)
		return VX_ERR_PIXEL_ASPECT;

	*out_par = (float)av_q2d(par);
	return VX_ERR_SUCCESS;
}