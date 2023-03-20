#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "libvx.h"

bool vx_is_packet_error(int result)
{
	return result != 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF;
}

static bool vx_is_rational_initialized(AVRational rational)
{
	return rational.num != 0 && rational.den != 1;
}

enum AVPixelFormat vx_to_av_pix_fmt(vx_pix_fmt fmt)
{
	enum AVPixelFormat formats[] = { AV_PIX_FMT_RGB24, AV_PIX_FMT_GRAY8, AV_PIX_FMT_BGRA };
	return formats[fmt];
}

enum AVSampleFormat vx_to_av_sample_fmt(vx_sample_fmt fmt)
{
	// Only return packed sample formats, planar formats are not supported in libvx
	return fmt == VX_SAMPLE_FMT_FLT
		? AV_SAMPLE_FMT_FLT
		: AV_SAMPLE_FMT_S16;
}

bool vx_rectangle_is_initialized(vx_rectangle rect)
{
	return (rect.x + rect.y + rect.width + rect.height) > 0;
}

bool vx_rectangle_contains(vx_rectangle a, vx_rectangle b)
{
	return (b.x + b.width) <= (a.x + a.width)
		&& (b.x) >= (a.x)
		&& (b.y) >= (a.y)
		&& (b.y + b.height) <= (a.y + a.height);
}

bool av_audio_params_equal(const struct av_audio_params a, const struct av_audio_params b)
{
	return av_channel_layout_compare(&a.channel_layout, &b.channel_layout) == 0
		&& a.sample_rate == b.sample_rate
		&& a.sample_format == b.sample_format
		&& a.time_base.den == b.time_base.den
		&& a.time_base.num == b.time_base.num;
}

struct av_audio_params av_audio_params_from_codec(const AVCodecContext* codec)
{
	struct av_audio_params params = {
		.channel_layout = codec->ch_layout,
		.sample_format = codec->sample_fmt,
		.sample_rate = codec->sample_rate,
		.time_base = codec->time_base
	};

	return params;
}

/// <summary>
/// Audio parameters from a frame. Optionally provide a backup time base to use
/// in case the value is not set on the frame.
/// </summary>
struct av_audio_params av_audio_params_from_frame(const AVFrame* frame, const AVRational* time_base)
{
	const AVRational time_base_corrected = time_base && !vx_is_rational_initialized(frame->time_base)
		? *time_base
		: frame->time_base;

	struct av_audio_params params = {
		.channel_layout = frame->ch_layout,
		.sample_format = frame->format,
		.sample_rate = frame->sample_rate,
		.time_base = time_base_corrected
	};

	return params;
}

/// <summary>
/// Video parameters from a frame. Optionally provide a backup time base to use
/// in case the value is not set on the frame.
/// </summary>
struct av_video_params av_video_params_from_frame(const AVFrame* frame, const AVRational* time_base)
{
	const AVRational time_base_corrected = time_base && !vx_is_rational_initialized(frame->time_base)
		? *time_base
		: frame->time_base;

	struct av_video_params params = {
		.width = frame->width,
		.height = frame->height,
		.pixel_format = frame->format,
		.sample_aspect_ratio = frame->sample_aspect_ratio,
		.time_base = time_base_corrected
	};

	return params;
}