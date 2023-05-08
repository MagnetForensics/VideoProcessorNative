#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>

#include "libvx.h"
#include "util.h"
#include "filter.h"
#include "filtergraph.h"

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

	av_log(NULL, AV_LOG_DEBUG, "Using HW pixel format for filter %i", format);

	return format;
}

vx_error vx_get_rotation_transform(const AVStream* stream, char** out_transform, char** out_transform_args)
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
			*out_transform_args = ROTATION_CLOCKWISE;
		}
		else if (theta > 45) {
			*out_transform = "transpose";
			*out_transform_args = ROTATION_COUNTERCLOCKWISE;
		}

		result = VX_ERR_SUCCESS;
	}
	else {
		result = VX_ERR_STREAM_INFO;
	}

	return result;
}

static vx_error vx_get_video_filter_args(struct av_video_params params, int args_length, char* out_args)
{
	snprintf(
		out_args,
		args_length,
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		params.width,
		params.height,
		params.pixel_format,
		params.time_base.num,
		params.time_base.den,
		params.sample_aspect_ratio.num,
		params.sample_aspect_ratio.den);

	av_log(NULL, AV_LOG_DEBUG, "Video filter args %s\n", out_args);

	return VX_ERR_SUCCESS;
}

static vx_error vx_get_audio_filter_args(struct av_audio_params params, int args_length, char* out_args)
{
	char layout[100];

	if (params.channel_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&params.channel_layout, params.channel_layout.nb_channels);

	av_channel_layout_describe(&params.channel_layout, layout, sizeof(layout));

	snprintf(
		out_args,
		args_length,
		"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s:channels=%d",
		params.time_base.num,
		params.time_base.den,
		params.sample_rate,
		av_get_sample_fmt_name(params.sample_format),
		layout,
		params.channel_layout.nb_channels);

	av_log(NULL, AV_LOG_DEBUG, "Audio filter args %s\n", out_args);

	return VX_ERR_SUCCESS;
}

vx_error vx_get_filter_args_from_codec(const AVCodecContext* codec, int args_length, char* out_args)
{
	vx_error result = VX_ERR_SUCCESS;

	if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
		// Set the correct pixel format, we need to find the format that a hardware frame will
		// be converted to after transferring to a software frame, but before converting via scaling
		enum AVPixelFormat format = codec->pix_fmt;
		if (codec->hw_device_ctx)
		{
			format = vx_get_hw_pixel_format(codec->hw_device_ctx);
			if (!format || format == AV_PIX_FMT_NONE) {
				av_log(codec, AV_LOG_ERROR, "Cannot find compatible hardware pixel format\n");
				return VX_ERR_INIT_FILTER;
			}
		}

		struct av_video_params params = {
			.width = codec->width,
			.height = codec->height,
			.sample_aspect_ratio = codec->sample_aspect_ratio,
			.pixel_format = format,
			.time_base = codec->time_base
		};
		result = vx_get_video_filter_args(params, args_length, out_args);
	}
	else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
		struct av_audio_params params = av_audio_params_from_codec(codec);

		result = vx_get_audio_filter_args(params, args_length, out_args);
	}
	else {
		result = VX_ERR_INIT_FILTER;
	}

	return result;
}

static vx_error vx_get_filter_args(const AVCodecContext* codec, void* params, int args_length, char* out_args)
{
	vx_error result = VX_ERR_INIT_FILTER;

	if (params) {
		if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			result = vx_get_audio_filter_args(*(struct av_audio_params*)params, args_length, out_args);
		}
		else if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			result = vx_get_video_filter_args(*(struct av_video_params*)params, args_length, out_args);
		}
	}
	else {
		result = vx_get_filter_args_from_codec(codec, args_length, out_args);
	}

	return result;
}

static vx_error vx_initialize_audiostats_filter(AVFilterContext** last_filter, const int* pad_index)
{
	char args[] = "metadata=1:reset=1:measure_overall=Peak_level+RMS_level+RMS_peak:measure_perchannel=0";

	return vx_filtergraph_insert_filter(last_filter, pad_index, "astats", NULL, args);
}

static vx_error vx_initialize_crop_filter(AVFilterContext** last_filter, const int* pad_index, vx_rectangle frame_area, vx_rectangle crop_area)
{
	int result = VX_ERR_UNKNOWN;
	char args[100];

	if (crop_area.width <= 0 || crop_area.height <= 0 || !vx_rectangle_contains(frame_area, crop_area)) {
		av_log(NULL, AV_LOG_FATAL, "The specified crop area is not valid for this video.\n");
		return VX_ERR_INIT_FILTER;
	}

	snprintf(
		args,
		sizeof(args),
		"w=%d:h=%d:x=%d:y=%d:exact=1",
		crop_area.width,
		crop_area.height,
		crop_area.x,
		crop_area.y);

	if (args) {
		if ((result = vx_filtergraph_insert_filter(last_filter, pad_index, "crop", NULL, args)) != VX_ERR_SUCCESS)
			return result;
	}

	return VX_ERR_SUCCESS;
}

static vx_error vx_initialize_scene_filter(AVFilterContext** last_filter, const int* pad_index, const float threshold)
{
	int result = VX_ERR_UNKNOWN;
	char args[100];

	snprintf(args, sizeof(args), "threshold=%f", threshold);

	if (args) {
		if ((result = vx_filtergraph_insert_filter(last_filter, pad_index, "scdet", NULL, args)) != VX_ERR_SUCCESS)
			return result;
	}

	return VX_ERR_SUCCESS;
}

static vx_error vx_initialize_rotation_filter(AVFilterContext** last_filter, const int* pad_index, const AVStream* stream)
{
	int result = VX_ERR_UNKNOWN;
	char* transform = NULL;
	char* args = NULL;
	const char* token = NULL;

	result = vx_get_rotation_transform(stream, &transform, &args);
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
			if (vx_filtergraph_insert_filter(last_filter, pad_index, token, NULL, args) != VX_ERR_SUCCESS)
				break;
			token = strtok(NULL, ",");
		}
		free(src);
	}

	return VX_ERR_SUCCESS;
}

static vx_error vx_initialize_audio_filters(AVFilterContext** last_filter, const int* pad_index)
{
	return vx_initialize_audiostats_filter(last_filter, pad_index);
}

static vx_error vx_initialize_video_filters(AVFilterContext** last_filter, const int* pad_index, const AVStream* stream, const vx_video_options options)
{
	vx_error result = VX_ERR_SUCCESS;

	if (options.autorotate)
		if ((result = vx_initialize_rotation_filter(last_filter, pad_index, stream)) != VX_ERR_SUCCESS)
			goto cleanup;

	if (vx_rectangle_is_initialized(options.crop_area)) {
		const vx_rectangle frame_area = { 0, 0, stream->codecpar->width, stream->codecpar->height };
		if ((result = vx_initialize_crop_filter(last_filter, pad_index, frame_area, options.crop_area)) != VX_ERR_SUCCESS)
			goto cleanup;
	}

	if (options.scene_threshold >= 0)
		if ((result = vx_initialize_scene_filter(last_filter, pad_index, options.scene_threshold)) != VX_ERR_SUCCESS)
			goto cleanup;

cleanup:
	return result;
}

/// <summary>
/// Set up the filter pipeline for either an audio or video stream.
/// Optionally specify the stream parameters as av_audio_params or av_video_params.
/// </summary>
vx_error vx_initialize_filtergraph(const vx_video* video, const enum AVMediaType type, void* params)
{
	vx_error result = VX_ERR_INIT_FILTER;
	bool is_video = type == AVMEDIA_TYPE_VIDEO;
	AVFilterContext* last_filter = NULL;
	char args[256] = { 0 };
	int pad_index = 0;

	if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create filter graph for %s media type\n", av_get_media_type_string(type));
		goto cleanup;
	}

	AVFilterGraph** filter_graph = is_video
		? &video->filter_pipeline
		: &video->filter_pipeline_audio;
	const AVCodecContext* codec = is_video
		? video->video_codec_ctx
		: video->audio_codec_ctx;
	const AVStream* stream = video->fmt_ctx->streams[is_video ? video->video_stream : video->audio_stream];

	if ((result = vx_get_filter_args(codec, params, sizeof(args), &args)) != VX_ERR_SUCCESS)
		return result;

	if (*filter_graph)
		avfilter_graph_free(filter_graph);

	// Create the pipeline
	if ((result = vx_filtergraph_init(filter_graph, type, &args)) != VX_ERR_SUCCESS || !*filter_graph)
		goto cleanup;

	last_filter = (*filter_graph)->filters[(*filter_graph)->nb_filters - 1];

	// Add filters
	if (is_video) {
		if ((result = vx_initialize_video_filters(&last_filter, &pad_index, stream, video->options)) != VX_ERR_SUCCESS)
			goto cleanup;
	}
	else {
		if ((result = vx_initialize_audio_filters(&last_filter, &pad_index)) != VX_ERR_SUCCESS)
			goto cleanup;
	}

	// Complete the pipeline with the added filters
	return vx_filtergraph_configure(filter_graph, type, &last_filter, &pad_index);

cleanup:
	if (result != VX_ERR_SUCCESS) {
		if (last_filter)
			avfilter_free(last_filter);

		if (*filter_graph)
			avfilter_graph_free(filter_graph);
	}

	return result;
}

vx_error vx_filter_frame(const vx_video* video, AVFrame* av_frame, const enum AVMediaType type)
{
	vx_error result = VX_ERR_SUCCESS;
	AVFilterGraph** filter_graph = type == AVMEDIA_TYPE_VIDEO
		? &video->filter_pipeline
		: &video->filter_pipeline_audio;
	void* params = NULL;

	av_log(NULL, AV_LOG_DEBUG, "Filtering frame at ts %i\n", av_frame->best_effort_timestamp);

	if (*filter_graph && (*filter_graph)->nb_filters > 1) {
		const AVFilterLink* filter_source = avfilter_graph_get_filter(*filter_graph, "in")->outputs[0];

		if (type == AVMEDIA_TYPE_AUDIO) {
			// Reinitialize the pipeline if the audio properties have changed
			const struct av_audio_params frame_audio_params = av_audio_params_from_frame(
				av_frame,
				&video->fmt_ctx->streams[video->audio_stream]->time_base);
			const struct av_audio_params filter_audio_params = {
				.channel_layout = filter_source->ch_layout,
				.sample_format = filter_source->format,
				.sample_rate = filter_source->sample_rate,
				.time_base = filter_source->time_base
			};

			if (!av_audio_params_equal(filter_audio_params, frame_audio_params)) {
				av_log(NULL, AV_LOG_DEBUG, "Reinitializing filtering for audio frame at ts %i\n", av_frame->best_effort_timestamp);
				params = &frame_audio_params;
			}
		}
		else if (type == AVMEDIA_TYPE_VIDEO) {
			// Reinitialize the pipeline if the frame size has changed
			const struct av_video_params frame_video_params = av_video_params_from_frame(av_frame, &video->fmt_ctx->streams[video->video_stream]->time_base);
			const struct av_video_params filter_video_params = {
				.width = filter_source->w,
				.height = filter_source->h,
				.pixel_format = filter_source->format,
				.sample_aspect_ratio = filter_source->sample_aspect_ratio,
				.time_base = filter_source->time_base
			};

			if (!av_video_params_equal(filter_video_params, frame_video_params)) {
				av_log(NULL, AV_LOG_DEBUG, "Reinitializing filtering for video frame at ts %i\n", av_frame->best_effort_timestamp);
				params = &frame_video_params;
			}
		}

		// Reinitialize the pipeline if the current audio or video properties have changed
		// from those used to initialize the filter graph
		if (params) {
			if ((result = vx_initialize_filtergraph(video, type, params) != VX_ERR_SUCCESS))
				goto cleanup;
		}

		result = vx_filtergraph_process_frame(filter_graph, av_frame);
	}

cleanup:
	return result;
}