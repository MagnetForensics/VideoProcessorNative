#ifndef LIBVX_FILTER_H
#define LIBVX_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

#define ROTATION_CLOCKWISE "dir=clock"
#define ROTATION_COUNTERCLOCKWISE "dir=cclock"

	vx_error vx_get_rotation_transform(const AVStream* stream, char** out_transform, char** out_transform_args);
	vx_error vx_get_audio_filter_args(struct av_audio_params params, int args_length, char* out_args);
	vx_error vx_get_video_filter_args(struct av_video_params params, int args_length, char* out_args);
	vx_error vx_initialize_filtergraph(const vx_video* video, const enum AVMediaType type, void* params);
	vx_error vx_filter_frame(const vx_video* video, AVFrame* av_frame, const enum AVMediaType type);

#ifdef __cplusplus
}
#endif

#endif
