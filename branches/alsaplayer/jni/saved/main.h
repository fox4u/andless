
#ifndef _MAIN_H_INCLUDED
#define _MAIN_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#if 1
#define log_info(fmt, args...)  __android_log_print(ANDROID_LOG_INFO, "liblossless", "[%s] " fmt, __func__, ##args)
#else
#define log_info(...)
#endif
#define log_err(fmt, args...)   __android_log_print(ANDROID_LOG_ERROR, "liblossless", "[%s] " fmt, __func__, ##args)

typedef struct {
   enum _playback_state_t {
	STATE_STOPPED = 0,	/* init state */
	STATE_PLAYING,
	STATE_PAUSED
   } state;
   int  track_time;	
   int  channels, samplerate, bps, written;
   int  block_min, block_max, frames;
   pthread_mutex_t mutex, pause_mutex;
   struct alsa_priv *apriv;
} playback_ctx;

extern int alsa_select_device(playback_ctx *ctx, int card, int device);
extern ssize_t alsa_write(playback_ctx *ctx, const void *buf, size_t count);
extern int alsa_start(playback_ctx *ctx);
extern void alsa_stop(playback_ctx *ctx);
extern bool alsa_pause(playback_ctx *ctx);
extern bool alsa_resume(playback_ctx *ctx);
extern bool alsa_set_volume(playback_ctx *ctx, int vol);
extern void alsa_close(playback_ctx *ctx);

#define LIBLOSSLESS_ERR_NOCTX		1
#define LIBLOSSLESS_ERR_INV_PARM	2
#define LIBLOSSLESS_ERR_NOFILE		3
#define LIBLOSSLESS_ERR_FORMAT		4
#define LIBLOSSLESS_ERR_AU_GETCONF 	5	
#define LIBLOSSLESS_ERR_AU_SETCONF	6
#define LIBLOSSLESS_ERR_AU_BUFF		7
#define LIBLOSSLESS_ERR_AU_SETUP	8
#define LIBLOSSLESS_ERR_AU_START	9
#define LIBLOSSLESS_ERR_IO_WRITE 	10	
#define LIBLOSSLESS_ERR_IO_READ		11
#define LIBLOSSLESS_ERR_DECODE		12 
#define LIBLOSSLESS_ERR_OFFSET		13
#define LIBLOSSLESS_ERR_NOMEM		14
#define LIBLOSSLESS_ERR_INIT		15

#ifdef __cplusplus
}
#endif

#endif
