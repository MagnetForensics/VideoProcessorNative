#ifndef LIBVX_FILTERS_H
#define LIBVX_FILTERS_H

#ifdef __cplusplus
extern "C" {
#endif

#define ROTATION_CLOCKWISE "dir=clock"
#define ROTATION_COUNTERCLOCKWISE "dir=cclock"

	vx_error vx_get_rotation_transform(const AVStream* stream, char** out_transform, char** out_transform_args);
	vx_error vx_initialize_filtergraph(const vx_video* video, const enum AVMediaType type, void* params);

#ifdef __cplusplus
}
#endif

#endif
