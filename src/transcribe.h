#ifndef LIBVX_TRANSCRIBE_H
#define LIBVX_TRANSCRIBE_H

#include "libvx.h"

#ifdef __cplusplus
extern "C" {
#endif

//typedef struct vx_transcription_segment vx_transcription_segment;
typedef struct vx_transcription_ctx vx_transcription_ctx;

typedef enum {
	greedy			= 1,
	beam_search		= 2
} vx_transcription_strategy;

typedef struct vx_transcription_segment
{
	int64_t ts_start;
	int64_t ts_end;
	char* text;
	int text_length;
	//char language[2];
	char* language;
} vx_transcription_segment;

// Initialize a transcription context with the specified model and strategy.
VX_DECLSPEC vx_transcription_ctx* VX_CDECL vx_transcription_init(char* model_path, vx_transcription_strategy strategy);

// Initialize audio resampler to convert input frames to the required audio format for transcription.
// Audio will also be filtered to reduce noise and enhance speech.
VX_DECLSPEC vx_error VX_CDECL vx_init_audio_conversion(vx_transcription_ctx* ctx, vx_video* video);

// Transcribe audio frames to text. Audio conversion must be initialized before transcribing directly from frames.
// Audio may be buffered internally until there is enough audio to feed the transcriber.
// Buffered audio and any remaining text can be retrieved by flushing the transcriber with a empty sample packet
VX_DECLSPEC vx_error VX_CDECL vx_transcribe(vx_transcription_ctx* ctx, AVFrame* frame, struct vx_transcription_segment* out_transcription);

// Transcribe raw audio samples to text. No resampling or filtering with be done so samples must be in the correct format for transcription.
// Samples may be buffered internally until there is enough audio to feed the transcriber.
// Buffered audio and any remaining text can be retrieved by flushing the transcriber with a empty sample packet.
VX_DECLSPEC vx_error VX_CDECL vx_transcribe_samples(vx_transcription_ctx* ctx, void* samples, int sample_count, struct vx_transcription_segment* out_transcription);

VX_DECLSPEC void VX_CDECL vx_transcription_free(vx_transcription_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
