#ifndef LIBVX_FILTERGRAPH_H
#define LIBVX_FILTERGRAPH_H

#include "libvx.h"

#ifdef __cplusplus
extern "C" {
#endif

	VX_DECLSPEC vx_error VX_CDECL vx_insert_filter(
		AVFilterContext** last_filter, int* pad_index, const char* filter_name, const char* filter_label, const char* args);
	VX_DECLSPEC vx_error VX_CDECL vx_filtergraph_init(AVFilterGraph** filter_graph, enum AVMediaType type, const char* args);
	VX_DECLSPEC vx_error VX_CDECL vx_filtergraph_configure(AVFilterGraph** filter_graph, enum AVMediaType mediaType, AVFilterContext** last_filter, int* pad_index);

#ifdef __cplusplus
}
#endif

#endif
