#ifndef LIBVX_UTIL_H
#define LIBVX_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

	bool vx_is_packet_error(int result);
	enum AVPixelFormat vx_to_av_pix_fmt(vx_pix_fmt fmt);
	enum AVSampleFormat vx_to_av_sample_fmt(vx_sample_fmt fmt);
	bool vx_rectangle_is_initialized(vx_rectangle rect);
	bool vx_rectangle_contains(vx_rectangle a, vx_rectangle b);
	bool av_audio_params_equal(const struct av_audio_params a, const struct av_audio_params b);
	struct av_audio_params av_audio_params_from_codec(const AVCodecContext* context);
	struct av_audio_params av_audio_params_from_frame(const AVFrame* frame, const AVRational* time_base);
	struct av_video_params av_video_params_from_frame(const AVFrame* frame, const AVRational* time_base);

#ifdef __cplusplus
}
#endif

#endif
