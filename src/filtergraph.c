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
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>

#include "libvx.h"
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

vx_error vx_insert_filter(AVFilterContext** last_filter, int* pad_index, const char* filter_name, const char* filter_label, const char* args)
{
	vx_error result = VX_ERR_INIT_FILTER;
	AVFilterGraph* graph = (*last_filter)->graph;
	AVFilterContext* filter_ctx;

	if (!filter_label)
		filter_label = filter_name;

	if (avfilter_graph_create_filter(&filter_ctx, avfilter_get_by_name(filter_name), filter_label, args, NULL, graph) < 0)
		return result;

	if (avfilter_link(*last_filter, *pad_index, filter_ctx, 0) < 0)
		return result;

	*last_filter = filter_ctx;
	*pad_index = 0;

	return VX_ERR_SUCCESS;
}

vx_error vx_get_filter_args(const vx_video* video, const enum AVMediaType type, int args_length, char* out_args)
{
	bool is_video = type == AVMEDIA_TYPE_VIDEO;

	const AVStream* stream = is_video
		? video->fmt_ctx->streams[video->video_stream]
		: video->fmt_ctx->streams[video->audio_stream];

	if (type == AVMEDIA_TYPE_VIDEO) {
		// Set the correct pixel format, we need to find the format that a hardware frame will
		// be converted to after transferring to system memory, but before converting via scaling
		enum AVPixelFormat format = video->video_codec_ctx->pix_fmt;
		if (video->hw_device_ctx)
		{
			format = vx_get_hw_pixel_format(video->hw_device_ctx);
			if (!format) {
				av_log(NULL, AV_LOG_ERROR, "Cannot find compatible pixel format\n");
				return VX_ERR_INIT_FILTER;
			}
		}

		snprintf(
			out_args,
			args_length,
			"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
			video->video_codec_ctx->width,
			video->video_codec_ctx->height,
			format,
			stream->time_base.num,
			stream->time_base.den,
			video->video_codec_ctx->sample_aspect_ratio.num,
			video->video_codec_ctx->sample_aspect_ratio.den);
	}
	else
	{
		if (video->audio_codec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
			av_channel_layout_default(&video->audio_codec_ctx->ch_layout, video->audio_codec_ctx->ch_layout.nb_channels);

		char layout[100];
		av_channel_layout_describe(&video->audio_codec_ctx->ch_layout, layout, sizeof(layout));

		snprintf(
			out_args,
			args_length,
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s:channels=%d",
			stream->time_base.num,
			stream->time_base.den,
			video->audio_codec_ctx->sample_rate,
			av_get_sample_fmt_name(video->audio_codec_ctx->sample_fmt),
			layout,
			video->audio_codec_ctx->ch_layout.nb_channels);
	}

	return VX_ERR_SUCCESS;
}

/// <summary>
/// Set up a filter graph ready for filters to be added
/// </summary>
vx_error vx_filtergraph_init(AVFilterGraph** filter_graph, enum AVMediaType type, const char* args)
{
	vx_error result = VX_ERR_INIT_FILTER;
	AVFilterContext* filter_source;

	if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create filter pipeline for this media type\n");
		goto cleanup;
	}

	const char* buffer_source = type == AVMEDIA_TYPE_VIDEO
		? "buffer"
		: "abuffer";

	*filter_graph = avfilter_graph_alloc();
	if (!*filter_graph) {
		result = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	// Create the filter pipeline source
	if (avfilter_graph_create_filter(&filter_source, avfilter_get_by_name(buffer_source), "in", args, NULL, *filter_graph) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto cleanup;
	}

	return VX_ERR_SUCCESS;

cleanup:
	return result;
}

/// <summary>
/// Complete the setup of a filter graph once filters have been added
/// </summary>
vx_error vx_filtergraph_configure(AVFilterGraph** filter_graph, enum AVMediaType mediaType, AVFilterContext** last_filter, int* pad_index)
{
	vx_error result = VX_ERR_INIT_FILTER;

	if (mediaType != AVMEDIA_TYPE_VIDEO && mediaType != AVMEDIA_TYPE_AUDIO) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create filter pipeline for this media type\n");
		goto cleanup;
	}

	const char* buffer_sink = mediaType == AVMEDIA_TYPE_VIDEO
		? "buffersink"
		: "abuffersink";

	if ((result = vx_insert_filter(last_filter, pad_index, buffer_sink, "out", NULL)) != VX_ERR_SUCCESS)
		goto cleanup;

	// Finally, construct the filter graph using all the linked nodes
	if (avfilter_graph_config(*filter_graph, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Could not configure filter graph\n");
		result = VX_ERR_INIT_FILTER;
	}

cleanup:
	return result;
}