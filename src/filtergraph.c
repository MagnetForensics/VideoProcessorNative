#include <stdlib.h>
#include <stdbool.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>

#include "libvx.h"
#include "util.h"
#include "filtergraph.h"

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
		av_log(NULL, AV_LOG_ERROR, "Cannot create filter graph for %s media type\n", av_get_media_type_string(type));
		goto cleanup;
	}

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
vx_error vx_filtergraph_configure(AVFilterGraph** filter_graph, enum AVMediaType type, AVFilterContext** last_filter, int* pad_index)
{
	vx_error result = VX_ERR_INIT_FILTER;

	if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create filter graph for %s media type\n", av_get_media_type_string(type));
		goto cleanup;
	}

	const char* buffer_sink = type == AVMEDIA_TYPE_VIDEO
		? "buffersink"
		: "abuffersink";

	if ((result = vx_filtergraph_insert_filter(last_filter, pad_index, buffer_sink, "out", NULL)) != VX_ERR_SUCCESS)
		goto cleanup;

	// Finally, construct the filter graph using all the linked nodes
	if (avfilter_graph_config(*filter_graph, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Could not configure filter graph\n");
		result = VX_ERR_INIT_FILTER;
	}

cleanup:
	return result;
}

vx_error vx_filtergraph_insert_filter(AVFilterContext** last_filter, int* pad_index, const char* filter_name, const char* filter_label, const char* args)
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

vx_error vx_filtergraph_process_frame(AVFilterGraph** filter_graph, AVFrame* av_frame)
{
	vx_error result = VX_ERR_UNKNOWN;
	int ret = 0;

	if (*filter_graph && (*filter_graph)->nb_filters > 1) {
		const AVFilterContext* filter_source = avfilter_graph_get_filter(*filter_graph, "in");
		const AVFilterContext* filter_sink = avfilter_graph_get_filter(*filter_graph, "out");

		if (!(filter_source && filter_sink)) {
			result = VX_ERR_UNKNOWN;
			goto cleanup;
		}

		if (av_buffersrc_add_frame_flags(filter_source, av_frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
			av_log(filter_source, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
			goto cleanup;
		}

		// The frame reference is being reused, so the old frame has to be cleaned up first
		av_frame_unref(av_frame);

		// More than one frame could be returned, depending on the filter graph layout or
		// how many frames were fed to the buffer source.
		// Only non-branching graphs are currently used, but this would need updating to handle
		// multiple frames if more complex graphs were used.
		while ((ret >= 0 || ret == AVERROR(EAGAIN)) && !av_frame->data[0]) {
			ret = av_buffersink_get_frame(filter_sink, av_frame);

			if (vx_is_packet_error(ret)) {
				av_log(filter_sink, AV_LOG_ERROR, "Error retrieving frames from the filtergraph\n");
				goto cleanup;
			}
		}
	}

	result = VX_ERR_SUCCESS;

cleanup:
	return result;
}