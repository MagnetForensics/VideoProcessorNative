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
#include "filter.h"
#include "filtergraph.h"
#include "util.h"
#include "whisper.h"

#ifdef __cplusplus
#pragma error
#endif

#define WHISPER_CHANNEL_COUNT 2
#define WHISPER_TEXT_MAX_LENGTH 255
#define WHISPER_LANGUAGE_MAX_LENGTH 2
#define WHISPER_HINT_TOKENS_COUNT 32
#define AUDIO_BUFFER_SECONDS 4
#define AUDIO_BUFFER_MAX_SAMPLES AUDIO_BUFFER_SECONDS * WHISPER_SAMPLE_RATE

struct vx_transcription_ctx {
	struct whisper_context* whisper_ctx;
	enum whisper_sampling_strategy strategy;

	AVFilterGraph* filter_graph;
	struct av_audio_params audio_params;
	uint8_t** audio_buffer;
	int sample_count;

	whisper_token hint_tokens[WHISPER_HINT_TOKENS_COUNT];
	int hint_token_count;
};

static enum whisper_sampling_strategy vx_to_whisper_strategy(vx_transcription_strategy strategy)
{
	enum whisper_sampling_strategy strategies[] = { WHISPER_SAMPLING_GREEDY, WHISPER_SAMPLING_BEAM_SEARCH };
	return strategies[strategy];
}

static vx_error vx_transcription_segment_init(vx_transcription_segment* segment)
{
	if (!segment)
		return VX_ERR_UNKNOWN;

	segment->ts_start = 0;
	segment->ts_end = 0;
	segment->text = calloc(WHISPER_TEXT_MAX_LENGTH + 1, sizeof(char));
	segment->length = 0;
	segment->language = calloc(WHISPER_LANGUAGE_MAX_LENGTH + 1, sizeof(char));

	return segment->text == NULL || segment->language == NULL
		? VX_ERR_ALLOCATE
		: VX_ERR_SUCCESS;
}

vx_transcription_segment* vx_transcription_buffer_init(int capacity)
{
	vx_transcription_segment* buffer = malloc(capacity * sizeof(vx_transcription_segment));

	if (buffer) {
		for (int i = 0; i < capacity; i++) {
			if (vx_transcription_segment_init(&buffer[i]) != VX_ERR_SUCCESS) {
				av_log(NULL, AV_LOG_ERROR, "Unable to allocate transcription segment.\n");
				break;
			}
		}
	}
	else {
		av_log(NULL, AV_LOG_ERROR, "Unable to allocate transcription buffer.\n");
	}

	return buffer;
}

/// <summary>
/// Create a new transcription context, returns NULL on error.
/// </summary>
struct vx_transcription_ctx* vx_transcription_init(const char* model_path, vx_transcription_strategy strategy)
{
	vx_transcription_ctx* ctx = calloc(1, sizeof(vx_transcription_ctx));
	if (!ctx)
		goto cleanup;

	// Audio input is assumed to be in the correct format, unless set via filtering
	ctx->audio_params = (struct av_audio_params)VX_TRANSCRIPTION_AUDIO_PARAMS;
	ctx->strategy = vx_to_whisper_strategy(strategy);

	int ret = av_samples_alloc_array_and_samples(
		&ctx->audio_buffer,
		NULL,
		ctx->audio_params.channel_layout.nb_channels,
		AUDIO_BUFFER_MAX_SAMPLES,
		ctx->audio_params.sample_format,
		0);

	if (ret < 0)
		goto cleanup;

	ctx->whisper_ctx = whisper_init_from_file(model_path);
	if (!ctx->whisper_ctx)
		goto cleanup;

	av_log(NULL, AV_LOG_INFO, whisper_print_system_info());

	return ctx;

cleanup:
	vx_transcription_free(ctx);
	av_log(NULL, AV_LOG_ERROR, "Unable to initialize transcription context.\n");

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

static vx_error vx_initialize_resample_filter(AVFilterContext** last_filter, const int* pad_index)
{
	char args[100];
	char layout[100];
	struct av_audio_params audio_params = VX_TRANSCRIPTION_AUDIO_PARAMS;

	av_channel_layout_describe(&audio_params.channel_layout, layout, sizeof(layout));

	snprintf(
		args,
		sizeof(args),
		"out_sample_rate=%i:out_chlayout=%s:out_sample_fmt=%s",
		audio_params.sample_rate,
		layout,
		av_get_sample_fmt_name(audio_params.sample_format));

	return vx_filtergraph_insert_filter(last_filter, pad_index, "aresample", NULL, args);
}

static vx_error vx_initialize_audio_filters(AVFilterContext** last_filter, const int* pad_index)
{
	vx_error result = VX_ERR_SUCCESS;

	// TODO: Remove distortion and noise
	// See afir and anlmdn
	// and/or highpass=f=200, lowpass=f=3000
	// TODO: Remove silence
	// see silenceremove
	// TODO: Enhance speech
	// See arnndn and https://github.com/GregorR/rnnoise-models
	// or afftdn nf=-25

	// Resample to Whisper compatible format
	// TODO: Convert to correct number of channels
	if ((result = vx_initialize_resample_filter(last_filter, pad_index)) != VX_ERR_SUCCESS)
		goto cleanup;

cleanup:
	return result;
}

vx_error vx_transcription_initialize_audio_conversion(vx_transcription_ctx** ctx, struct av_audio_params params)
{
	vx_error result = VX_ERR_SUCCESS;
	AVFilterGraph** filter_graph = &(*ctx)->filter_graph;
	AVFilterContext* last_filter = NULL;
	char filter_args[256] = { 0 };
	int pad_index = 0;

	if ((*ctx)->filter_graph)
		avfilter_graph_free(filter_graph);

	(*ctx)->audio_params = params;

	if ((result = vx_get_audio_filter_args(params, sizeof(filter_args), &filter_args)) != VX_ERR_SUCCESS)
		goto end;

	if ((result = vx_filtergraph_init(filter_graph, AVMEDIA_TYPE_AUDIO, &filter_args) || !*filter_graph) != VX_ERR_SUCCESS)
		goto end;

	last_filter = (*filter_graph)->filters[(*filter_graph)->nb_filters - 1];

	if ((result = vx_initialize_audio_filters(&last_filter, &pad_index)) != VX_ERR_SUCCESS)
		goto end;

	if ((result = vx_filtergraph_configure(filter_graph, AVMEDIA_TYPE_AUDIO, &last_filter, &pad_index)) != VX_ERR_SUCCESS)
		goto end;

end:
	return result;
}

vx_error vx_transcribe_frame(vx_transcription_ctx** ctx, AVFrame* frame, vx_transcription_segment** out_transcription, int* out_count)
{
	vx_error result = VX_ERR_SUCCESS;
	if (!frame || frame->nb_samples <= 0)
		return VX_ERR_NO_AUDIO;

	if (!(*ctx)->filter_graph) {
		av_log(NULL, AV_LOG_ERROR, "Audio filtering must be initialized before transcribing frames.\n");
		return VX_ERR_RESAMPLE_AUDIO;
	}

	struct av_audio_params frame_audio_params = av_audio_params_from_frame(frame, NULL);
	if (!av_audio_params_equal((*ctx)->audio_params, frame_audio_params)) {
		av_log(NULL, AV_LOG_WARNING, "Frame audio parameters did not match the expected format for transcription.\n Attempting to re-initialize audio conversion.");
		if (vx_transcription_initialize_audio_conversion(ctx, frame_audio_params) != VX_ERR_SUCCESS) {
			return VX_ERR_RESAMPLE_AUDIO;
		}
	}

	if ((result = vx_filtergraph_process_frame(&(*ctx)->filter_graph, frame)) != VX_ERR_SUCCESS)
		return result;

	result = vx_transcribe_samples(ctx, (const uint8_t**)frame->data, frame->nb_samples, out_transcription, out_count);

	return result;
}

vx_error vx_transcribe_samples(vx_transcription_ctx** ctx, const uint8_t** samples, int sample_count, vx_transcription_segment** out_transcription, int* out_count)
{
	vx_error result = VX_ERR_SUCCESS;
	*out_count = 0;
	struct av_audio_params audio_params = VX_TRANSCRIPTION_AUDIO_PARAMS;

	if (!*out_transcription || out_count <= 0) {
		av_log(NULL, AV_LOG_ERROR, "Unable to transcribe audio samples, output buffer is not initialized or does not have sufficient capacity.\n");
		return VX_ERR_UNKNOWN;
	}

	if ((*ctx)->sample_count + sample_count < AUDIO_BUFFER_MAX_SAMPLES) {
		// Buffer audio if there is not enough samples for transcription
		int copy_result = av_samples_copy(
			(*ctx)->audio_buffer,
			(const uint8_t* const*)samples,
			(*ctx)->sample_count,
			0,
			sample_count,
			audio_params.channel_layout.nb_channels,
			audio_params.sample_format);
		
		if (copy_result < 0)
			return VX_ERR_RESAMPLE_AUDIO;

		(*ctx)->sample_count += sample_count;

		return VX_ERR_SUCCESS;
	}

	// Transcribe audio
	struct whisper_full_params params = whisper_full_default_params((*ctx)->strategy);
	params.language = "auto";
	params.print_progress = false;
	params.n_threads = 8; // TODO: Set dynamically
	params.duration_ms = 0; // Use all the provided samples
	params.no_context = true;
	params.audio_ctx = 0;
	params.token_timestamps = true;
	params.max_tokens = 32;
	params.prompt_tokens = (*ctx)->hint_tokens;
	params.prompt_n_tokens = (*ctx)->hint_token_count;

	// Whisper processes the audio in 1 second chunks but anything smaller will be discarded
	// Save any remaining samples to be processed with the next batch
	// TODO: Handle processing of all remaining samples on last frame. Pad with silence?
	int samples_to_keep = (*ctx)->sample_count % audio_params.sample_rate;
	int samples_to_process = (*ctx)->sample_count - samples_to_keep;

	// For packed sample formats, only the first data plane is used, and samples for each channel are interleaved.
	// In this case, linesize is the buffer size, in bytes, for the 1 plane
	int whisper_result = whisper_full((*ctx)->whisper_ctx, params, (const float*)(*ctx)->audio_buffer[0], samples_to_process * audio_params.channel_layout.nb_channels);

	if (whisper_result == 0) {
		int keep_ms = 200;
		const int n_segments = whisper_full_n_segments((*ctx)->whisper_ctx);

		for (int i = 0; i < n_segments; i++) {
			vx_transcription_segment* segment = &(*out_transcription)[i];
			const char* text = whisper_full_get_segment_text((*ctx)->whisper_ctx, i);

			const int64_t t0 = params.token_timestamps ? whisper_full_get_segment_t0((*ctx)->whisper_ctx, i) : 0;
			const int64_t t1 = params.token_timestamps ? whisper_full_get_segment_t1((*ctx)->whisper_ctx, i) : 0;
			const char* language = whisper_lang_str(whisper_full_lang_id((*ctx)->whisper_ctx));

			// Timestamps in milliseconds
			segment->ts_start = max((t0 * 10) - keep_ms, 0);
			segment->ts_end = max((t1 * 10) - keep_ms, 0);
			strcpy_s(segment->text, WHISPER_TEXT_MAX_LENGTH + 1, text);
			segment->length = (int)strlen(text);
			strcpy_s(segment->language, WHISPER_LANGUAGE_MAX_LENGTH + 1, language != NULL ? language : "");

			(*out_count)++;
		}

		// Keep the last few samples to mitigate word boundary issues
		samples_to_keep += (int)(((double)keep_ms / 1000) * audio_params.sample_rate);

		if (samples_to_keep > 0)
			av_samples_copy((*ctx)->audio_buffer, (const uint8_t* const*)(*ctx)->audio_buffer, 0, (*ctx)->sample_count - samples_to_keep, samples_to_keep, audio_params.channel_layout.nb_channels, audio_params.sample_format);
		av_samples_set_silence((*ctx)->audio_buffer, samples_to_keep, (audio_params.sample_rate * AUDIO_BUFFER_SECONDS) - samples_to_keep, audio_params.channel_layout.nb_channels, audio_params.sample_format);
		(*ctx)->sample_count = samples_to_keep;

		// Add tokens of the last full length segment as the prompt
		memset((*ctx)->hint_tokens, 0, sizeof((*ctx)->hint_tokens));
		(*ctx)->hint_token_count = 0;

		if (n_segments > 0) {
			int last_segment = n_segments - 1;
			const int token_count = whisper_full_n_tokens((*ctx)->whisper_ctx, last_segment);
			const int token_offset = token_count - min(token_count, params.max_tokens);
			for (int j = token_offset; j < min(token_count, params.max_tokens); ++j) {
				(*ctx)->hint_tokens[j] = whisper_full_get_token_id((*ctx)->whisper_ctx, last_segment, j);
				(*ctx)->hint_token_count++;
			}
		}
	}

	// Finally, store the samples that would not fit
	av_samples_copy((*ctx)->audio_buffer, (const uint8_t* const*)samples, (*ctx)->sample_count, 0, sample_count, audio_params.channel_layout.nb_channels, audio_params.sample_format);
	(*ctx)->sample_count += sample_count;

	return result;
}

static void vx_transcription_segment_free(vx_transcription_segment* segment)
{
	if (segment) {
		if (segment->text)
			free(segment->text);

		if (segment->language)
			free(segment->language);

		free(segment);
	}
}

void vx_transcription_buffer_free(vx_transcription_segment** buffer, int capacity)
{
	for (int i = 0; i < capacity; i++) {
		vx_transcription_segment_free(buffer[i]);
	}
}

void vx_transcription_free(vx_transcription_ctx** ctx)
{
	if (ctx) {
		if ((*ctx)->whisper_ctx)
			free(&(*ctx)->whisper_ctx);

		if ((*ctx)->audio_buffer)
			free(&(*ctx)->audio_buffer);

		free(ctx);
	}
}
