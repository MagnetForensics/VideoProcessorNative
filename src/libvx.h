#ifndef LIBVX_H
#define LIBVX_H

#ifdef __cplusplus
extern "C" {
#endif

#define VX_DECLSPEC __declspec(dllexport)
#define VX_CDECL __cdecl

typedef struct vx_video vx_video;
typedef struct vx_video_options vx_video_options;
typedef struct vx_frame vx_frame;
typedef struct vx_frame_info vx_frame_info;
typedef struct vx_rectangle vx_rectangle;

typedef enum {
	VX_LOG_NONE    = 0,
	VX_LOG_FATAL   = 1,
	VX_LOG_ERROR   = 2,
	VX_LOG_WARNING = 3,
	VX_LOG_INFO    = 4,
	VX_LOG_DEBUG   = 5
} vx_log_level;

typedef enum {
	VX_PIX_FMT_RGB24 = 0,
	VX_PIX_FMT_GRAY8 = 1,
	VX_PIX_FMT_RGB32 = 2,
} vx_pix_fmt;

typedef enum {
	VX_SAMPLE_FMT_S16 = 0,
	VX_SAMPLE_FMT_FLT = 1
} vx_sample_fmt;

typedef enum {
	VX_ERR_FRAME_DEFERRED  = -1,
	VX_ERR_SUCCESS         = 0,
	VX_ERR_UNKNOWN         = 1,
	VX_ERR_ALLOCATE        = 2,
	VX_ERR_FRAME_RATE      = 3,
	VX_ERR_OPEN_FILE       = 4,
	VX_ERR_STREAM_INFO     = 5,
	VX_ERR_VIDEO_STREAM    = 6,
	VX_ERR_FIND_CODEC      = 7,
	VX_ERR_OPEN_CODEC      = 8,
	VX_ERR_EOF             = 9,
	VX_ERR_DECODE_VIDEO    = 10,
	VX_ERR_SCALING         = 11,
	VX_ERR_PIXEL_ASPECT    = 12,
	VX_ERR_DECODE_AUDIO    = 13,
	VX_ERR_NO_AUDIO        = 14,
	VX_ERR_RESAMPLE_AUDIO  = 15,
	VX_ERR_FILE_NOT_FOUND  = 16,
	VX_ERR_INIT_FILTER     = 17,
} vx_error;

typedef enum {
	VX_FF_KEYFRAME			= 1 << 0,
	VX_FF_BYTE_POS_GUESSED	= 1 << 1,
	VX_FF_HAS_PTS			= 1 << 2,
	VX_FF_DEFERRED			= 1 << 3
} vx_frame_flag;

typedef enum {
	VX_HW_ACCEL_ALL  = 1 << 0,
	VX_HW_ACCEL_720  = 1 << 1,
	VX_HW_ACCEL_1080 = 1 << 2,
	VX_HW_ACCEL_1440 = 1 << 3,
	VX_HW_ACCEL_2160 = 1 << 4,
	VX_HW_ACCEL_HEVC = 1 << 5,
	VX_HW_ACCEL_H264 = 1 << 6
} vx_hwaccel_flag;

typedef void (*vx_audio_callback)(const void* samples, int num_samples, double ts, void* user_data);
typedef void (*vx_log_callback)(const char* message, int level);

VX_DECLSPEC void VX_CDECL vx_log_set_cb(vx_log_callback cb);

VX_DECLSPEC vx_error VX_CDECL vx_open(vx_video** video, const char* filename, const vx_video_options options);
VX_DECLSPEC void VX_CDECL vx_close(vx_video* video);

VX_DECLSPEC int VX_CDECL vx_get_width(const vx_video* video);
VX_DECLSPEC int VX_CDECL vx_get_height(const vx_video* video);

VX_DECLSPEC int VX_CDECL vx_get_audio_present(const vx_video* video);
VX_DECLSPEC int VX_CDECL vx_get_audio_sample_rate(const vx_video* video);
VX_DECLSPEC int VX_CDECL vx_get_audio_channels(const vx_video* video);

VX_DECLSPEC vx_error VX_CDECL vx_set_audio_params(vx_video* me, int sample_rate, int channels, vx_sample_fmt format, vx_audio_callback cb, void* user_data);
VX_DECLSPEC vx_error VX_CDECL vx_set_audio_max_samples_per_frame(vx_video* me, int max_samples);

VX_DECLSPEC long long VX_CDECL vx_get_file_position(const vx_video* video);
VX_DECLSPEC long long VX_CDECL vx_get_file_size(const vx_video* video);
VX_DECLSPEC double VX_CDECL vx_timestamp_to_seconds(const vx_video* video, const long long ts);

// Note that you need to re-open the file (create a new vx_video instance) after counting frames.
VX_DECLSPEC vx_error VX_CDECL vx_count_frames(vx_video* me, int* out_num_frames);

VX_DECLSPEC vx_error VX_CDECL vx_get_pixel_aspect_ratio(const vx_video* video, float* out_par);
VX_DECLSPEC vx_error VX_CDECL vx_get_frame_rate(const vx_video* video, float* out_fps);
VX_DECLSPEC vx_error VX_CDECL vx_get_duration(const vx_video* video, float* out_duration);
VX_DECLSPEC bool VX_CDECL vx_get_hw_context_present(const vx_video* video);

VX_DECLSPEC vx_frame* VX_CDECL vx_frame_create(int width, int height, vx_pix_fmt pix_fmt);
VX_DECLSPEC void VX_CDECL vx_frame_destroy(vx_frame* frame);

VX_DECLSPEC vx_error VX_CDECL vx_frame_step(vx_video* video, vx_frame_info frame_info);
VX_DECLSPEC vx_error VX_CDECL vx_frame_transfer_data(const vx_video* video, vx_frame* frame);
VX_DECLSPEC void* VX_CDECL vx_frame_get_buffer(vx_frame* frame);
VX_DECLSPEC int VX_CDECL vx_frame_get_buffer_size(const vx_frame* frame);

#ifdef __cplusplus
}
#endif

#endif
