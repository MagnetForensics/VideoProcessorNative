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

vx_error vx_get_video_filter_args(struct av_video_params params, int args_length, char* out_args)
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

	return VX_ERR_SUCCESS;
}

vx_error vx_get_audio_filter_args(struct av_audio_params params, int args_length, char* out_args)
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

	return VX_ERR_SUCCESS;
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

/// <summary>
/// Set up a filter graph ready for filters to be added
/// </summary>
vx_error vx_filtergraph_init(AVFilterGraph** filter_graph, enum AVMediaType type, const char* args)
{
	vx_error result = VX_ERR_INIT_FILTER;
	AVFilterContext* filter_source;
	const char* buffer_source = type == AVMEDIA_TYPE_VIDEO
		? "buffer"
		: "abuffer";

	if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
		av_log(*filter_graph, AV_LOG_ERROR, "Cannot create filter pipeline for this media type\n");
		goto cleanup;
	}

	if (*filter_graph)
		avfilter_graph_free(filter_graph);

	*filter_graph = avfilter_graph_alloc();
	if (!*filter_graph) {
		result = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	// Create the filter pipeline source
	if (avfilter_graph_create_filter(&filter_source, avfilter_get_by_name(buffer_source), "in", args, NULL, *filter_graph) < 0) {
		av_log(filter_source, AV_LOG_ERROR, "Cannot create buffer source\n");
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
		av_log(*filter_graph, AV_LOG_ERROR, "Cannot create filter pipeline for this media type\n");
		goto cleanup;
	}

	const char* buffer_sink = mediaType == AVMEDIA_TYPE_VIDEO
		? "buffersink"
		: "abuffersink";

	if ((result = vx_insert_filter(last_filter, pad_index, buffer_sink, "out", NULL)) != VX_ERR_SUCCESS)
		goto cleanup;

	// Finally, construct the filter graph using all the linked nodes
	if (avfilter_graph_config(*filter_graph, NULL) < 0) {
		av_log(*filter_graph, AV_LOG_ERROR, "Could not configure filter graph\n");
		result = VX_ERR_INIT_FILTER;
	}

cleanup:
	return result;
}