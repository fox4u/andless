#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>
#ifdef ANDROID
#include <android/log.h>
#endif
#include <jni_sub.h>

#include <errno.h>
#include <signal.h>

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>

#include "main.h"


typedef struct _alsa_priv {
    int  card, device;
    int  pcm_fd, ctl_fd;
    struct snd_ctl_elem_list elist;
    struct snd_ctl_elem_id *eid;
    int supp_formats_mask;
    int supp_rates_mask;	
    int setup_info;
    int msm_mm_idx;	/* X in MultiMediaX for msm devices */
    void *pcm_buf;
    int buf_bytes;
} alsa_priv;

static const playback_format_t supp_formats[] = {
    { SNDRV_PCM_FORMAT_S16_LE,  (1 << 0), 16, 16, "SNDRV_PCM_FORMAT_S16_LE" },
    { SNDRV_PCM_FORMAT_S24_LE,  (1 << 1), 32, 24, "SNDRV_PCM_FORMAT_S24_LE" },
    { SNDRV_PCM_FORMAT_S24_3LE, (1 << 2), 24, 24, "SNDRV_PCM_FORMAT_S24_3LE" },
    { SNDRV_PCM_FORMAT_S32_LE,  (1 << 3), 32, 32, "SNDRV_PCM_FORMAT_S32_LE" },
};
#define n_supp_formats (sizeof(supp_formats))/(sizeof(supp_formats[0]))

static const struct _supp_rates {
    int rate, mask;
} supp_rates[] = {
    { 44100, (1 << 0) }, { 88200, (1 << 1) }, { 176400, (1 << 2) }, /* to be up/down-sampled to 48 kHz on msm devices */
    { 48000, (1 << 3) }, { 96000, (1 << 4) }, { 192000, (1 << 5) }
};
#define n_supp_rates (sizeof(supp_rates))/(sizeof(supp_rates[0]))


/* Standard names of mixer controls and their values to be set/reset for MSM8974 chips. First two to be changed depending on media. */
static char *msm_names[] = {
   "SLIM_0_RX Format", "SLIM_0_RX SampleRate", "SLIM_0_RX Channels", "SLIM RX1 MUX", "SLIM RX2 MUX", "RX1 MIX1 INP1", "RX2 MIX1 INP1",
   "CLASS_H_DSM MUX", "HPHL DAC Switch", "HPHL Volume", "HPHR Volume", 
   0 /* placeholder for "SLIMBUS_0_RX Audio Mixer MultiMediaX" */, 0
};
static char *msm_values[] = {
    0, 0, "Two", "AIF1_PB", "AIF1_PB", "RX1", "RX2",
    "DSM_HPHL_RX1", "1", "15", "15", 
    "1", 0
};		
#define MSM_FMT_IDX	0
#define MSM_RATE_IDX	1
#define MSM_INIT_IDX	11

void alsa_close(playback_ctx *ctx) 
{
    alsa_priv *priv;	
	if(!ctx) return;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(!priv) return;		
	if(priv->pcm_fd >= 0) close(priv->pcm_fd);
	if(priv->ctl_fd >= 0) close(priv->ctl_fd);
	if(priv->eid) free(priv->eid);
	if(priv->pcm_buf) free(priv->pcm_buf);
	memset(ctx->alsa_priv, 0, sizeof(alsa_priv));
	priv->ctl_fd = -1;
	priv->pcm_fd = -1;
	priv->card = -1;
	priv->device = -1;
}

static int set_mixer_control(playback_ctx *ctx, char *name, char *value)
{
    int i, k;
    struct snd_ctl_elem_info ei, ei1;
    struct snd_ctl_elem_value ev;
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;

	if(priv->ctl_fd < 0) return -1;
	for(k = 0; k < priv->elist.count; k++) {
	    /* get info for each control id */
	    memset(&ei, 0, sizeof(ei));
	    ei.id.numid = priv->eid[k].numid;
	    if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &ei) < 0) {
	    /*    log_err("cannot get info for control id %d\n", ei.id.numid); */
		continue;
	    }
	    if(strcmp((char *)ei.id.name, name) == 0) break;
	}
	if(k == priv->elist.count) {
	    log_err("control %s not found", name);	
	    return -1;
	}
	memset(&ev, 0, sizeof(ev));
	ev.id.numid = ei.id.numid;

	switch(ei.type) {
	    case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
	    case SNDRV_CTL_ELEM_TYPE_INTEGER:
		for(k = 0; k < ei.count; k++) ev.value.integer.value[k] = atoi(value);
		if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) {
		    log_err("failed to set value for %s", name);
		    return -1;
		} else {
		    log_info("%s -> %s", name, value);
		    return 0;
		}
	    case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		memset(&ei1, 0, sizeof(ei1));
		ei1.id.numid = ei.id.numid;
		for(k = 0; k < ei.value.enumerated.items; k++) {  /* find enum index j that refers to our value */
		    ei1.value.enumerated.item = k;
		    if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &ei1) < 0) {
			log_err("cannot get name for value %d for enum control %s", k, name);
		        return -1;
		    }
		    if(strcmp(ei1.value.enumerated.name, value) == 0) {  /* that's it! */
			for(i = 0; i < ei.count; i++) ev.value.enumerated.item[i] = k;
			if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) {
			    log_err("failed to set value for %s", name);
			    return -1;	
			} else {
			    log_info("%s -> %s", name, value);
			    return 0;
			}
		    }
		}
		log_err("failed to find value %s among those permitted for %s", value, name);
		return -1;
	    default:
		log_err("cannot handle control %s of type %d", name, ei.type);
		return -1;
	}
}

#define param_to_interval(p,n)	(&(p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]))
#define param_to_mask(p,n)	(&(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]))

static inline void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned int bit)
{
    struct snd_mask *m = param_to_mask(p, n);
	m->bits[0] = 0;
	m->bits[1] = 0;
	m->bits[bit >> 5] |= (1 << (bit & 31));
}

static inline void param_set_int(struct snd_pcm_hw_params *p, int n, unsigned int val)
{
    struct snd_interval *i = param_to_interval(p, n);
	i->min = val;
	i->max = val;
	i->integer = 1;
}

static inline void param_set_range(struct snd_pcm_hw_params *p, int n, unsigned int min, unsigned int max)
{
    struct snd_interval *i = param_to_interval(p, n);
	i->min = min;
	i->max = max;
	i->integer = 1;
}

static void param_init(struct snd_pcm_hw_params *p)
{
    int n;
    memset(p, 0, sizeof(*p));
    for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK;
         n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
            struct snd_mask *m = param_to_mask(p, n);
            m->bits[0] = ~0;
            m->bits[1] = ~0;
    }
    for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
         n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
            struct snd_interval *i = param_to_interval(p, n);
            i->min = 0;
            i->max = ~0;
    }
    p->rmask = ~0U;
    p->cmask = 0;
    p->info = ~0U;
}

static int apply_mixer_controls(playback_ctx *ctx, int start) 
{
    alsa_priv *priv;
    char tmp[128];
    int i;

	if(!ctx || !ctx->alsa_priv) return -1;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(priv->card != 0) return 0;
#ifdef ANDROID
	if(start) {
	    log_info("trying to set controls for built-in card");	
	    msm_values[MSM_FMT_IDX] = (ctx->bps == 16) ? "S16_LE" : "S24_LE";
	    switch(ctx->samplerate) {
		case 192000:	msm_values[MSM_RATE_IDX] = "KHZ_192"; break;	
		case 96000:	msm_values[MSM_RATE_IDX] = "KHZ_96"; break;	
		default:	msm_values[MSM_RATE_IDX] = "KHZ_48"; break;	
	    }
	    sprintf(tmp, "SLIMBUS_0_RX Audio Mixer MultiMedia%d", priv->msm_mm_idx);
	    msm_names[MSM_INIT_IDX] = tmp;
	    for(i = 0; msm_names[i]; i++) set_mixer_control(ctx, msm_names[i], msm_values[i]);
	    alsa_set_volume(ctx, ctx->volume, 1);
	} else {
	    sprintf(tmp, "SLIMBUS_0_RX Audio Mixer MultiMedia%d", priv->msm_mm_idx);
	    set_mixer_control(ctx, "SLIM_0_RX Format", "S16_LE");
	    set_mixer_control(ctx, "SLIM_0_RX SampleRate", "KHZ_48");
	    set_mixer_control(ctx, tmp, "0");
	}
#endif
    return 0;
}

static inline void setup_hwparams(struct snd_pcm_hw_params *params, 
	int fmt, int rate, int channels, int periods, int period_size) 
{
    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    if(fmt) param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, fmt);		/* we don't support U8 = 0 */
    if(rate) param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, rate);
    if(channels) param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS, channels);	
    if(period_size) param_set_int(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_size);
    if(periods) param_set_int(params, SNDRV_PCM_HW_PARAM_PERIODS, periods);
}

int alsa_select_device(playback_ctx *ctx, int card, int device) 
{
    char tmp[128];
    int  k, fd;
    struct snd_pcm_hw_params hwparams;
    alsa_priv *priv;

	log_info("switching to card %d device %d", card, device);

	if(!ctx) return LIBLOSSLESS_ERR_NOCTX;
	priv = (alsa_priv *) ctx->alsa_priv;

	if(!ctx->alsa_priv) {
	    ctx->alsa_priv = calloc(1, sizeof(alsa_priv));
	    if(!ctx->alsa_priv) return LIBLOSSLESS_ERR_NOMEM;
	    priv = (alsa_priv *) ctx->alsa_priv;	
	    priv->ctl_fd = -1;
	    priv->pcm_fd = -1;
	    priv->card = -1;
	    priv->device = -1;
	} else if(priv->card == card && priv->device == device) {
	    log_info("card/device unchanged");
	    return 0;	
	} else alsa_close(ctx);
	
	log_info("trying to open mixer");
	sprintf(tmp, "/dev/snd/controlC%d", card);	
	priv->ctl_fd = open(tmp, O_RDWR); 	    	

	if(priv->ctl_fd < 0) {
	    log_err("cannot open %s", tmp);
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_SETUP;
	}
	if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &priv->elist) < 0) {
	    log_err("cannot get number of controls");
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_GETCONF;
	}
	log_info("mixer opened, control count: %d", priv->elist.count);
	priv->eid = calloc(priv->elist.count, sizeof(struct snd_ctl_elem_id));
	if(!priv->eid) {
	    log_err("no memory\n");
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_NOMEM;
	}
	priv->elist.space = priv->elist.count;
	priv->elist.pids = priv->eid;
	if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &priv->elist) < 0) {
	    log_err("cannot get list of controls");
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_GETCONF;
	}
#ifdef ANDROID
	if(card == 0) { /* Uff. */
	    char mix_ctl[128];
	    struct snd_pcm_info pcm_info;
		log_info("trying to set controls for built-in card, device %d", device);
		memset(&pcm_info, 0, sizeof(pcm_info));
		pcm_info.device = device;
		if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_PCM_INFO, &pcm_info) != 0) {
		    log_err("failed to find pcm info for device %d", device);
		    alsa_close(ctx);
		    return LIBLOSSLESS_ERR_AU_GETCONF;
		}
		log_info("device %d: %s", device, pcm_info.id);
		if(sscanf((char *)pcm_info.id, "MultiMedia%d", &k) != 1) {
		    log_err("%d: not a multimedia device", device);
		    alsa_close(ctx);
		    return LIBLOSSLESS_ERR_INV_PARM;
		}
		priv->msm_mm_idx = k;
		sprintf(mix_ctl, "SLIMBUS_0_RX Audio Mixer MultiMedia%d", k);
		k = set_mixer_control(ctx, mix_ctl, "1");
	}
#endif
	sprintf(tmp, "/dev/snd/pcmC%dD%dp", card, device);

	for(k = 0; k < n_supp_formats; k++) {	
	    fd = open(tmp, O_RDWR);
	    setup_hwparams(&hwparams, supp_formats[k].fmt, 0, 0, 0, 0);
	    if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hwparams) == 0) 
		priv->supp_formats_mask |= supp_formats[k].mask;
	    close(fd);	
	}
	for(k = 0; k < n_supp_rates; k++) {
	    fd = open(tmp, O_RDWR);	
	    setup_hwparams(&hwparams, 0, supp_rates[k].rate, 0, 0, 0);	
            if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hwparams) == 0) 
		priv->supp_rates_mask |= supp_rates[k].mask;
	    close(fd);
	}

	if(!priv->supp_rates_mask || !priv->supp_formats_mask) {
	    log_err("unsupported hardware (format/rate masks=0x%x/0x%x)",
		priv->supp_formats_mask, priv->supp_rates_mask);
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_GETCONF;	
	}
	priv->card = card;
	priv->device = device;
	log_info("selected card=%d device=%d (format/rate masks=0x%x/0x%x)", 
		card, device, priv->supp_formats_mask, priv->supp_rates_mask);

    return 0;
}

int alsa_start(playback_ctx *ctx) 
{
    char tmp[128];
    struct snd_pcm_hw_params hwparams, *params = &hwparams;
    struct snd_pcm_sw_params swparams;
    int i, k, ret = 0;
    int periods_min, periods_max, persz_min, persz_max;
    alsa_priv *priv;

	if(!ctx || !ctx->alsa_priv) return LIBLOSSLESS_ERR_INV_PARM;
	priv = (alsa_priv *) ctx->alsa_priv;
#if 0
	if(ctx->block_min != ctx->block_max) {
	    log_err("variable block size not supported");
	    return LIBLOSSLESS_ERR_AU_SETUP;		
	}
	log_info("entry: block_max %d", ctx->block_max);
#endif

	for(i = 0; i < n_supp_formats; i++)
	    if(supp_formats[i].strm_bits == ctx->bps && 
		(supp_formats[i].mask & priv->supp_formats_mask)) break;
	if(i == n_supp_formats) {
	    log_err("device does not support %d-bit files", ctx->bps);
	    return LIBLOSSLESS_ERR_AU_SETUP;		
	}
	ctx->format = &supp_formats[i];
	for(i = 0; i < n_supp_rates; i++)
	    if(supp_rates[i].rate == ctx->samplerate &&
		(supp_rates[i].mask & priv->supp_rates_mask)) break;
	if(i == n_supp_rates) {
	    log_err("device does not support samplerate %d", ctx->bps);
	    return LIBLOSSLESS_ERR_AU_SETUP;		
	}
	apply_mixer_controls(ctx, 1);

	log_info("opening pcm");
	sprintf(tmp, "/dev/snd/pcmC%dD%dp", priv->card, priv->device);
	priv->pcm_fd = open(tmp, O_RDWR);
	if(priv->pcm_fd < 0) {
	    log_err("cannot open %s", tmp);
	    return LIBLOSSLESS_ERR_AU_SETUP;
	}
	log_info("pcm opened");

	setup_hwparams(params, ctx->format->fmt, ctx->samplerate, ctx->channels, 0, 0);

 	log_info("Trying: format=%s rate=%d channels=%d bps=%d (phys=%d)", ctx->format->str, 
	    ctx->samplerate, ctx->channels, ctx->bps, ctx->format->phys_bits);	

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_HW_REFINE, params) != 0) {
	    log_info("refine failed");
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
#if 1
	/* sanity check */
	i = param_to_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->max;
	if(i != ctx->format->phys_bits ||
		i != param_to_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->min) {
	    log_info("bogie refine");
     	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
#endif	
	periods_max = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIODS)->max;
	periods_min = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIODS)->min;
	persz_max = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max;
	persz_min = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;

	log_info("Period size: min=%d\tmax=%d", persz_min, persz_max);
	log_info("    Periods: min=%d\tmax=%d", periods_min, periods_max);

	ctx->periods = periods_max;
	ctx->period_size = persz_max;

	param_set_int(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, ctx->period_size);
	param_set_int(params, SNDRV_PCM_HW_PARAM_PERIODS, ctx->periods);

	/* Try to obtain the largest buffer possible keeping in mind 
	   that ALSA always tries to set minimum latency */

	if(ctx->periods > 16) ctx->periods = 16; 

#define NSTEPS	8

	i = (persz_max - persz_min) / NSTEPS; 

	while(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, params) < 0) {
	    ctx->period_size -= i;
	    if(ctx->period_size < persz_min || i == 0) {
		ctx->period_size = persz_max;
		ctx->periods >>= 1;	
	    } else if(ctx->period_size - i < persz_min && (i/NSTEPS) > 0) { /* Refine last step */
		log_info("refine");
		ctx->period_size += i; /* undo */	
		i /= NSTEPS;
		ctx->period_size -= i;		
	    }
	    if(ctx->periods < periods_min) {
		log_err("cannot set hw parameters");
		ret = LIBLOSSLESS_ERR_AU_SETCONF;
		goto err_exit;
	    }
	    setup_hwparams(params, ctx->format->fmt, ctx->samplerate, ctx->channels, ctx->periods, 0);
	    param_set_range(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, ctx->period_size, persz_max);		
	    log_info("retrying with period_size %d periods %d", ctx->period_size, ctx->periods);	
	}
	ctx->period_size = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max;
	priv->setup_info = params->info;

	log_info("selecting period size %d, periods %d [hw_info=0x%08X]",
		ctx->period_size, ctx->periods, priv->setup_info);

	priv->buf_bytes = ctx->period_size * ctx->channels * ctx->format->phys_bits/8;
	priv->pcm_buf = malloc(priv->buf_bytes);
	if(!priv->pcm_buf) {
	    log_err("no memory for buffer");
	    ret = LIBLOSSLESS_ERR_NOMEM;
	    goto err_exit;	
	}
        memset(priv->pcm_buf, 0, priv->buf_bytes);

	memset(&swparams, 0, sizeof(swparams));
	swparams.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	swparams.period_step = 1;
	swparams.avail_min = 1;
	swparams.start_threshold = ctx->period_size;
	swparams.stop_threshold = ctx->period_size * ctx->periods + 4;
	swparams.xfer_align = ctx->period_size / 2;
	swparams.silence_size = 0;
	swparams.silence_threshold = 0;
	swparams.boundary = ctx->period_size * ctx->periods;

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_SW_PARAMS, &swparams) < 0) {
	    log_err("falied to set sw parameters");
	    ret = LIBLOSSLESS_ERR_AU_SETCONF;
	    goto err_exit;
	}

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            log_err("prepare() failed");
            ret = LIBLOSSLESS_ERR_AU_SETCONF;
            goto err_exit;
        }

	for(k = 0; k < ctx->periods; k++) {
	    i = write(priv->pcm_fd, priv->pcm_buf, priv->buf_bytes);
	    if(i != priv->buf_bytes) {
		log_err("cannot fill initial buffer");
		ret = LIBLOSSLESS_ERR_AU_BUFF;
		goto err_exit;
	    }
	}
	ctx->written = 0;
	log_info("setup complete");
	return 0;

    err_exit:	
	if(priv->pcm_buf) {
	    free(priv->pcm_buf);
	    priv->pcm_buf = 0;	
	    priv->buf_bytes = 0;
	}
	if(priv->pcm_fd >= 0) close(priv->pcm_fd);
	priv->pcm_fd = -1;
	log_err("exiting on error");

    return ret;
}

void *alsa_get_buffer(playback_ctx *ctx) {
    return ((alsa_priv *) ctx->alsa_priv)->pcm_buf; 	
}

ssize_t alsa_write(playback_ctx *ctx, size_t count)
{
    alsa_priv *priv;
    const playback_format_t *pfmt;
    int i, written = 0;
    struct snd_xferi xf;
    struct snd_pcm_status pcm_stat;

	if(!ctx || !ctx->alsa_priv || ctx->state != STATE_PLAYING) {
	    log_err("stream must be closed or paused");	
	    return 0;
	}
	priv = (alsa_priv *) ctx->alsa_priv;	
	pfmt = ctx->format;
	
	if(count > ctx->period_size) {
	    log_err("frames count %d larger than period size %d", (int) count, ctx->period_size);
	    count = ctx->period_size;
	} else if(count < ctx->period_size) {
	    log_info("short buffer, must be EOF");	
	    memset(priv->pcm_buf + count * ctx->channels * pfmt->phys_bits/8, 0, 
			(ctx->period_size - count) * ctx->channels * pfmt->phys_bits/8);
	}	

	xf.buf = priv->pcm_buf;
	xf.frames = ctx->period_size;

	while(written < ctx->period_size) {
	    i = ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xf);
#if 0
	    if(!i && ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_STATUS, &pcm_stat) == 0 && pcm_stat.hw_ptr == pcm_stat.appl_ptr) 
		log_info("underrun to occur: %ld %ld %ld %ld", pcm_stat.hw_ptr, pcm_stat.appl_ptr, pcm_stat.avail, pcm_stat.avail_max);
#endif
	    if(i != 0) {
		switch(errno) {
		   case EINTR:
			log_info("exiting on EINTR");
			break;
		   case EAGAIN:
			log_err("EAGAIN");
			usleep(1000);
			break;
		   case EPIPE:
			log_info("underrun!");
			ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_STATUS, &pcm_stat);
			log_info("%ld %ld %ld %ld", pcm_stat.hw_ptr, pcm_stat.appl_ptr, pcm_stat.avail, pcm_stat.avail_max);
			if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
			    log_err("prepare failed after underrun");
			    ctx->alsa_error = 1;
			    return 0;	
			}
			break;
		   default:
			log_info("exiting on %s (%d)", strerror(errno), errno);
			buffer_stop(ctx->buff, 1);
			ctx->alsa_error = 1;
			return 0;		
		}	
	    }
	    written += xf.result;
	}	
	ctx->written += count;

    return written;

#if 0
	for(k = 0, written = 0; k < ctx->periods; k++) {
	    while(written < count) {
		i = write(priv->pcm_fd, priv->pcm_buf + k * ctx->period_size * ctx->channels * pfmt->phys_bits/8,
			ctx->period_size * ctx->channels * pfmt->phys_bits/8);
		if(i != ctx->period_size * ctx->channels * pfmt->phys_bits/8) {
		    switch(errno) {
			case EINTR:
			case EAGAIN:
			    log_info("ioctl error: %s", strerror(errno));
			    usleep(100);
			    break;
			case EPIPE:
			    if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
				log_err("prepare failed after underrun");
				return 0;	
			    }
			    log_info("underrun occurred");
			    break;
			default:
			    log_info("unhandled error %d in WRITEI_FRAMES: %s (%d)", i, strerror(errno), errno);
			    return 0;		
		    }
		} else written += ctx->period_size;
	    }
	}
#endif
}

void thread_exit(int j) {
    log_info("signal %d received",j);	
}

void *alsa_write_thread(void *a) 
{
    playback_ctx *ctx = (playback_ctx *) a;
    int i, k, f2b = ctx->channels * ctx->format->phys_bits/8;
    alsa_priv *priv;
    sigset_t set;
    struct sigaction sact = { .sa_handler = thread_exit,  };
 	
	if(!ctx) {
	    log_err("no ctx, exiting");	
	    return 0;
	}
	priv = (alsa_priv *) ctx->alsa_priv;
	if(!priv) {
	    log_err("no alsa ctx, exiting");
	    ctx->audio_thread = 0;
	    return 0;		
	}
	log_info("entering");
	sigaction(SIGUSR1, &sact, 0);
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &set, 0);
	while(1) {
	    k = check_state(ctx, __func__);
	    if(k < 0) break;
	    k = buffer_get(ctx->buff, priv->pcm_buf, ctx->period_size * f2b);
	    if(k <= 0) {
		log_info("buffer stopped or empty, exiting");
		break;
	    }
	    if(priv->pcm_fd < 0) {	/* should never happen, just in case... */
		log_info("pcm closed");
		break;
	    } 	
	    i = alsa_write(ctx, k/f2b);
	    if(i <= 0 || k != ctx->period_size * f2b) {
	   	log_info("eof detected, exiting");
		return 0;
	    }
	}
	ctx->audio_thread = 0;
	log_info("exiting");
    return 0;	
}

void alsa_stop(playback_ctx *ctx) 
{
    alsa_priv *priv;	

	if(!ctx || !ctx->alsa_priv) return;
	priv = (alsa_priv *) ctx->alsa_priv;
// test
//	apply_mixer_controls(ctx, 0);	
	if(priv->pcm_fd >= 0) {
	    log_info("stopping alsa");	
#if 0
	    if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_DROP) < 0) 
		log_info("pcm_drop failed");
	    else log_info("pcm_drop: success");	
#endif
	    close(priv->pcm_fd);
	    log_info("alsa stopped");
	}
// test	
//	apply_mixer_controls(ctx, 0);	
	priv->pcm_fd = -1;
	if(priv->pcm_buf) free(priv->pcm_buf);
	priv->pcm_buf = 0;
}

static bool alsa_pause_ioctl(playback_ctx *ctx, int push) 
{
    alsa_priv *priv;	
  	if(!ctx || !ctx->alsa_priv) return false;
	priv = (alsa_priv *) ctx->alsa_priv;
	if((priv->setup_info & SNDRV_PCM_INFO_PAUSE) == 0) {
	    log_err("pause/resume not supported by hardware");
	    return false;
	}		
    return ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PAUSE, push) == 0;
}

bool alsa_pause(playback_ctx *ctx) 
{
    apply_mixer_controls(ctx, 0);	
    return alsa_pause_ioctl(ctx, 1);
}

bool alsa_resume(playback_ctx *ctx) 
{
    apply_mixer_controls(ctx, 1);	
    return alsa_pause_ioctl(ctx, 0);
}


bool alsa_set_volume(playback_ctx *ctx, int vol, int force_now) 
{
    alsa_priv *priv;
    char tmp[128];

	if(!ctx || !ctx->alsa_priv) return false;	
	if(vol < 0 || vol > 100) {
	    log_err("value %d out of range", vol);
	    return false;
	}
	priv = (alsa_priv *) ctx->alsa_priv;

	if(ctx->state != STATE_PLAYING && !force_now) {
	    log_info("saved volume %d", vol);	
	    ctx->volume = vol; /* just save it. */
	    return true; 	
	} else if(priv->ctl_fd < 0)  log_err("mixer not open");
	  else if(priv->card != 0) log_err("don't know how to control volume of non-builtin card");
	  else {
	    int v = ctx->samplerate <= 48000 ? (vol * 83)/100 : (vol * 123)/100;
		sprintf(tmp, "%d", v);	
		if(set_mixer_control(ctx, "RX1 Digital Volume", tmp) == 0
		    && set_mixer_control(ctx, "RX2 Digital Volume", tmp) == 0) {
			ctx->volume = vol;	
		    	return true;
		}			 	
	 }
     return false; 
}


