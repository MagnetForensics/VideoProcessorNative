#ifndef LIBVX_TRANSCRIBE_H
#define LIBVX_TRANSCRIBE_H

#include "libvx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VX_TRANSCRIPTION_AUDIO_PARAMS { AV_CHANNEL_LAYOUT_STEREO, AV_SAMPLE_FMT_FLTP, WHISPER_SAMPLE_RATE, { 0, 1 } }

//typedef struct vx_transcription_segment vx_transcription_segment;
typedef struct vx_transcription_ctx vx_transcription_ctx;

typedef enum {
	/// <summary>
	/// Always select the most probable token
	/// </summary>
	VX_STRATEGY_GREEDY			= 0,
	/// <summary>
	/// Select the most probable token over at each time step (experimental)
	/// </summary>
	VX_STRATEGY_BEAM_SEARCH		= 1
} vx_transcription_strategy;

typedef struct vx_transcription_segment
{
	int64_t ts_start;
	int64_t ts_end;
	char* text;
	int text_length;
	//char language[2]; // whisper.cpp supports this from v1.20
	char* language;
} vx_transcription_segment;

	/// <summary>
	/// Initialize a transcription context with the specified model and strategy.
	/// </summary>
	/// <param name="model_path">The location for the transcription model on disk</param>
	/// <param name="strategy">Sampling strategy used for transcription</param>
	/// <returns>An initialized transcription context</returns>
	vx_transcription_ctx* vx_transcription_init(char* model_path, vx_transcription_strategy strategy);

	/// <summary>
	/// Initialize audio resampler to convert input frames to the required audio format for transcription.
	/// Audio will also be filtered to reduce noise and enhance speech.
	/// </summary>
	vx_error vx_transcription_initialize_audio_conversion(vx_transcription_ctx** ctx, struct av_audio_params params);

	/// <summary>
	/// Transcribe audio frames to text. Audio conversion must be initialized before transcribing directly from frames.
	/// Audio may be buffered internally until there is enough audio to feed the transcriber.
	/// Buffered audio and any remaining text can be retrieved by flushing the transcriber with a empty (NULL) frame.
	/// </summary>
	/// <remarks>
	/// Audio samples will be resampled to the correct format and filtered to enahance speech.
	/// The transcription context must already be correctly initialized with the expected input format.
	/// </remarks>
	/// <param name="ctx">A pre-initialized transcription context</param>
	/// <param name=frame">Audio frame input</param>
	/// <param name="out_transcription">The transcribed audio content</param>
	vx_error vx_transcribe_frame(vx_transcription_ctx** ctx, AVFrame* frame, vx_transcription_segment** out_transcription, int* out_count);

	/// <summary>
	/// Transcribe raw audio samples to text.
	/// Samples may be buffered internally until there is enough audio to feed the transcriber.
	/// Buffered audio and any remaining text can be retrieved by flushing the transcriber with an empty (NULL) sample packet.
	/// </summary>
	/// <remarks>
	/// No resampling or filtering with be performed unless specified, samples must be in the correct format.
	/// </remarks>
	/// <param name="ctx">A pre-initialized transcription context</param>
	/// <param name="samples">Planar audio samples</param>
	/// <param name="sample_count">The number of samples per channel</param>
	/// <param name="out_transcription">The transcribed audio content</param>
	vx_error vx_transcribe_samples(vx_transcription_ctx** ctx, const uint8_t** samples, int sample_count, vx_transcription_segment** out_transcription, int* out_count);

	void vx_transcription_free(vx_transcription_ctx** ctx);

#ifdef __cplusplus
}
#endif

#endif
