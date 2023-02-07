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
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "transcribe.h"
#include "whisper.h"

#ifdef __cplusplus
#pragma error
#endif

#define HINT_TOKENS_COUNT 32
#define AUDIO_BUFFER_SECONDS 4
#define WHISPER_CHANNEL_COUNT 1

struct vx_transcription_ctx {
	struct whisper_context* whisper_ctx;
	enum whisper_sampling_strategy strategy;

	AVFilterGraph* filter_pipeline;
	SwrContext* resampling_ctx;
	float* audio_buffer;
	//uint8_t** audio_buffer;
	int sample_count;

	whisper_token hint_tokens[HINT_TOKENS_COUNT];
	int hint_token_count;

	void* logging_cb;
};

static enum whisper_sampling_strategy vx_to_whisper_strategy(vx_transcription_strategy strategy)
{
	enum whisper_sampling_strategy strategies[] = { WHISPER_SAMPLING_GREEDY, WHISPER_SAMPLING_BEAM_SEARCH };
	return strategies[strategy];
}

/// <summary>
/// Create a new transcription context, returns NULL on error.
/// </summary>
struct vx_transcription_ctx* vx_transcription_init(const char* model_path, vx_transcription_strategy strategy)
{
	vx_transcription_ctx* ctx = malloc(sizeof(vx_transcription_ctx));
	if (!ctx)
		goto cleanup;

	ctx->strategy = vx_to_whisper_strategy(strategy);

	ctx->audio_buffer = malloc(WHISPER_SAMPLE_RATE * AUDIO_BUFFER_SECONDS * sizeof(float));

	if (!ctx->audio_buffer)
		goto cleanup;

	ctx->whisper_ctx = whisper_init(model_path);

	if (!ctx->whisper_ctx)
		goto cleanup;

	char* sys_info = whisper_print_system_info();
	printf("%s", sys_info);

	return ctx;

cleanup:
	vx_transcription_free(ctx);

	return NULL;
}

static vx_error vx_insert_filter(AVFilterContext** last_filter, int* pad_index, const char* filter_name, const char* filter_label, const char* args)
{
	vx_error result = VX_ERR_INIT_FILTER;
	AVFilterGraph* graph = (*last_filter)->graph;
	AVFilterContext* filter_ctx;
	if (!filter_label) {
		filter_label = filter_name;
	}

	if (avfilter_graph_create_filter(&filter_ctx, avfilter_get_by_name(filter_name), filter_label, args, NULL, graph) < 0)
		return result;

	if (avfilter_link(*last_filter, *pad_index, filter_ctx, 0) < 0)
		return result;

	*last_filter = filter_ctx;
	*pad_index = 0;

	return VX_ERR_SUCCESS;
}

vx_error vx_init_audio_conversion(vx_transcription_ctx* ctx, vx_video* video)
{

}

/// <summary>
/// Transcribe audio frames to text. Audio conversion must be initialized before transcribing directly from frames.
/// Audio may be buffered internally until there is enough audio to feed the transcriber.
/// Buffered audio and any remaining text can be retrieved by flushing the transcriber with a empty sample packet.
/// </summary>
vx_error vx_transcribe(vx_transcription_ctx* ctx, AVFrame* frame, struct vx_transcription_segment* out_transcription)
{
	//enum AVSampleFormat sample_format = vx_to_av_sample_fmt(video->options.audio_params.sample_format);
	enum AVSampleFormat sample_format = AV_SAMPLE_FMT_FLT;
	int max_samples = WHISPER_SAMPLE_RATE * AUDIO_BUFFER_SECONDS;

	if (ctx->sample_count + frame->nb_samples < max_samples) {
		// Buffer audio if there is not enough for transcription
		av_samples_copy(ctx->audio_buffer, (const uint8_t* const*)frame->data, ctx->sample_count, 0, frame->nb_samples, WHISPER_CHANNEL_COUNT, sample_format);
		ctx->sample_count += frame->nb_samples;
	}
	else {
		vx_error ret = vx_transcribe_samples(ctx, frame->buf, frame->nb_samples, out_transcription);

		if (ret != VX_ERR_SUCCESS)
			return ret;
	}
}

/// <summary>
/// Transcribe raw audio samples to text. No resampling or filtering with be done so samples must be in the correct format for transcription.
/// An error will be returned if there are not enough samples for transcription.
/// </summary>
vx_error vx_transcribe_samples(vx_transcription_ctx* ctx, void* samples, int sample_count, struct vx_transcription_segment* out_transcription)
{
	if (sample_count < 1 * WHISPER_SAMPLE_RATE) {
		return VX_ERR_UNKNOWN;
	}

	// Transcribe audio
	//struct whisper_full_params params = whisper_full_default_params(ctx->strategy);
	//params.language = "auto";
	//params.print_progress = false;
	//params.n_threads = 8;
	//params.duration_ms = 0; // Use all the provided samples
	//params.no_context = true;
	//params.audio_ctx = 0;
	//params.token_timestamps = true;
	//params.max_tokens = 32;
	//params.prompt_tokens = video->transcription_hints;
	//params.prompt_n_tokens = video->transcription_hints_count;

	//// Whisper processes the audio in 1 second chunks but anything smaller will be discarded
	//// Save any remaining samples to be processed with the next batch
	//// TODO: Handle processing of all remaining samples on last frame. Pad with silence?
	//int samples_to_keep = video->sample_count % video->options.audio_params.sample_rate;
	//int samples_to_process = video->sample_count - samples_to_keep;

	//// For packed sample formats, only the first data plane is used, and samples for each channel are interleaved.
	//// In this case, linesize is the buffer size, in bytes, for the 1 plane
	//int whisper_result = whisper_full(video->whisper_ctx, params, (const float*)video->audio_buffer[0], samples_to_process);

	//if (whisper_result == 0) {
	//	int keep_ms = 200;
	//	const int n_segments = whisper_full_n_segments(video->whisper_ctx);

	//	for (int i = 0; i < n_segments; i++) {
	//		vx_audio_transcription* transcription = &frame->audio_info.transcription[i];
	//		const char* text = whisper_full_get_segment_text(video->whisper_ctx, i);

	//		const int64_t t0 = params.token_timestamps ? whisper_full_get_segment_t0(video->whisper_ctx, i) : 0;
	//		const int64_t t1 = params.token_timestamps ? whisper_full_get_segment_t1(video->whisper_ctx, i) : 0;

	//		// Timestamps in milliseconds
	//		transcription->ts_start = max((t0 * 10) - keep_ms, 0);
	//		transcription->ts_end = max((t1 * 10) - keep_ms, 0);
	//		strcpy_s(transcription->text, 256 + 1, text);
	//		transcription->text_length = (int)strlen(text);
	//		//transcription->language = whisper_full_lang_id(video->whisper_ctx); // TODO: Available in latest version of whisper.cpp
	//		strcpy_s(transcription->language, 2 + 1, "en");

	//		dprintf(text);
	//	}

	//	// Keep the last few samples to mitigate word boundary issues
	//	samples_to_keep += (int)(((double)keep_ms / 1000) * video->options.audio_params.sample_rate);

	//	if (samples_to_keep > 0)
	//		av_samples_copy(video->audio_buffer, (const uint8_t* const*)video->audio_buffer, 0, video->sample_count - samples_to_keep, samples_to_keep, video->options.audio_params.channels, sample_format);
	//	av_samples_set_silence(video->audio_buffer, samples_to_keep, (video->options.audio_params.sample_rate * AUDIO_BUFFER_SECONDS) - samples_to_keep, video->options.audio_params.channels, sample_format);
	//	video->sample_count = samples_to_keep;

	//	// Add tokens of the last full length segment as the prompt
	//	memset(video->transcription_hints, 0, sizeof(video->transcription_hints));
	//	video->transcription_hints_count = 0;

	//	if (n_segments > 0) {
	//		int last_segment = n_segments - 1;
	//		const int token_count = whisper_full_n_tokens(video->whisper_ctx, last_segment);
	//		const int token_offset = token_count - min(token_count, params.max_tokens);
	//		for (int j = token_offset; j < min(token_count, params.max_tokens); ++j) {
	//			video->transcription_hints[j] = (whisper_full_get_token_id(video->whisper_ctx, last_segment, j));
	//			video->transcription_hints_count++;
	//		}
	//	}
	//}

	//// Finally, store the samples that would not fit
	//av_samples_copy(video->audio_buffer, (const uint8_t* const*)frame->audio_buffer, video->sample_count, 0, frame->audio_sample_count, video->options.audio_params.channels, sample_format);
	//video->sample_count += frame->audio_sample_count;
}

void vx_transcription_free(vx_transcription_ctx* ctx)
{
	if (ctx) {
		if (ctx->whisper_ctx)
			free(&ctx->whisper_ctx);
		if (ctx->audio_buffer)
			free(&ctx->audio_buffer);

		free(&ctx);
	}
}
