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
#include <android/log.h>
#include <errno.h>

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>

#include "main.h"

struct alsa_priv {
    int  card, device;
    int  pcm_fd, ctl_fd;
    struct snd_ctl_elem_list elist;
    struct snd_ctl_elem_id *eid;
    int supp_formats_mask;
    int supp_rates_mask;	
    int setup_info;
    int msm_mm_idx;	/* X in MultiMediaX for msm devices */
    int fmt_idx;	/* index into struct supp_formats */
    int periods;
    int period_size;	
    void *pcm_buf;
};

static const struct playback_format_t {
    snd_pcm_format_t fmt;
    int mask;	
    int phys_bits;
    int strm_bits;
    const char *str;
} supp_formats[] = {
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
   "CLASS_H_DSM MUX", "HPHL DAC Switch", "HPHL Volume", "HPHR Volume", "RX1 Digital Volume", "RX2 Digital Volume",   
   0 /* placeholder for "SLIMBUS_0_RX Audio Mixer MultiMediaXX" */, 0
};
static char *msm_values[] = {
    0, 0, "Two", "AIF1_PB", "AIF1_PB", "RX1", "RX2",
    "DSM_HPHL_RX1", "1", "15", "15", "120", "120", 
    "1", 0
};		
#define MSM_FMT_IDX	0
#define MSM_RATE_IDX	1
#define MSM_INIT_IDX	13

void alsa_close(playback_ctx *ctx) 
{
    struct alsa_priv *priv;	
	if(!ctx) return;
	priv = ctx->apriv;
	if(!priv) return;		
	if(priv->pcm_fd >= 0) close(priv->pcm_fd);
	if(priv->ctl_fd >= 0) close(priv->ctl_fd);
	if(priv->eid) free(priv->eid);
	if(priv->pcm_buf) free(priv->pcm_buf);
	memset(ctx->apriv, 0, sizeof(struct alsa_priv));
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
    struct alsa_priv *priv = ctx->apriv;

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

int alsa_select_device(playback_ctx *ctx, int card, int device) 
{
    char tmp[128];
    int  k, fd;
    struct snd_pcm_hw_params *params = 0;
    struct alsa_priv *priv;

	log_info("switching to card %d device %d", card, device);
	if(!ctx) return LIBLOSSLESS_ERR_NOCTX;
	if(!ctx->apriv) {
	    ctx->apriv = (struct alsa_priv *) calloc(1, sizeof(struct alsa_priv));
	    if(!ctx->apriv) return LIBLOSSLESS_ERR_NOMEM;
	    ctx->apriv->ctl_fd = -1;
	    ctx->apriv->pcm_fd = -1;
	    ctx->apriv->card = -1;
	    ctx->apriv->device = -1;
	} else if(ctx->apriv->card == card && ctx->apriv->device == device) {
	    log_info("card/device unchanged");
	    return 0;	
	} else alsa_close(ctx);
	
	priv = ctx->apriv;

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
	params = calloc(1, sizeof(struct snd_pcm_hw_params));
	if(!params) {
	    log_err("no memory for hwparams");	
	    alsa_close(ctx);	
	    return LIBLOSSLESS_ERR_NOMEM;
	}
	sprintf(tmp, "/dev/snd/pcmC%dD%dp", card, device);

#if 0
	fd = open(tmp, O_RDWR);
	if(fd < 0) {
		free(params);
		log_err("cannot open %s", tmp);
		alsa_close(ctx);
		return LIBLOSSLESS_ERR_AU_GETCONF;
	}	

	param_init(params);
	k = ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, params);
	close(fd)	
	if(k) {
	    free(params);	
	    log_err("initial SNDRV_PCM_IOCTL_HW_REFINE failed\n");
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_GETCONF;
	}
#endif

	for(k = 0; k < n_supp_formats; k++) {	
	    fd = open(tmp, O_RDWR);
	    param_init(params);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, supp_formats[k].fmt);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	    if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, params) == 0) 
		priv->supp_formats_mask |= supp_formats[k].mask;
	    close(fd);	
	}
	for(k = 0; k < n_supp_rates; k++) {
	    fd = open(tmp, O_RDWR);	
	    param_init(params);
// test only
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, supp_rates[k].rate);
            if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, params) == 0) 
		priv->supp_rates_mask |= supp_rates[k].mask;
	    close(fd);
	}
	free(params);

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
    struct snd_pcm_hw_params *params = 0;
    int i, k, ret = 0, periods_min, periods_max, persz_min, persz_max;
    struct alsa_priv *priv;
    const struct playback_format_t *pfmt;

	if(!ctx || !ctx->apriv) return LIBLOSSLESS_ERR_INV_PARM;
	priv = ctx->apriv;

	/* The simpler the better. Is there any vbs-capable encoder at all? */
	if(ctx->block_min != ctx->block_max) {
	    log_err("variable block size not supported");
	    return LIBLOSSLESS_ERR_AU_SETUP;		
	}
	log_info("entry: block_max %d", ctx->block_max);

	for(i = 0; i < n_supp_formats; i++)
	    if(supp_formats[i].strm_bits == ctx->bps && 
		(supp_formats[i].mask & priv->supp_formats_mask)) break;
	if(i == n_supp_formats) {
	    log_err("device does not support %d-bit files", ctx->bps);
	    return LIBLOSSLESS_ERR_AU_SETUP;		
	}
	priv->fmt_idx = i;
	pfmt = &supp_formats[i];

	for(i = 0; i < n_supp_rates; i++)
	    if(supp_rates[i].rate == ctx->samplerate &&
		(supp_rates[i].mask & priv->supp_rates_mask)) break;
	if(i == n_supp_rates) {
	    log_err("device does not support samplerate %d", ctx->bps);
	    return LIBLOSSLESS_ERR_AU_SETUP;		
	}
	if(priv->card == 0) {
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
	}
	log_info("opening pcm");
	sprintf(tmp, "/dev/snd/pcmC%dD%dp", priv->card, priv->device);
	priv->pcm_fd = open(tmp, O_RDWR);
	if(priv->pcm_fd < 0) {
	    log_err("cannot open %s", tmp);
	    return LIBLOSSLESS_ERR_AU_SETUP;
	}
	log_info("pcm opened");
	params = calloc(1, sizeof(struct snd_pcm_hw_params));
	if(!params) {
	    log_err("no memory for hwparams");	
	    ret = LIBLOSSLESS_ERR_NOMEM;
	    goto err_exit;	
	}
	param_init(params);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, pfmt->fmt);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS, ctx->channels);
	param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, ctx->samplerate);
	param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, pfmt->phys_bits);

 	log_info("Trying to refine: format=%s rate=%d channels=%d bps=%d (phys=%d)", pfmt->str, 
	    ctx->samplerate, ctx->channels, ctx->bps, pfmt->phys_bits);	

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_HW_REFINE, params) == 0) {
	    log_info("refine succeeded");
#define DUMP_INTERVAL(NAME, PARAMS, IVAL)       do {    \
   log_info(NAME ": min=%d\tmax=%d\n", param_to_interval(PARAMS, IVAL)->min, param_to_interval(PARAMS, IVAL)->max);       \
        } while(0)
	    DUMP_INTERVAL("Period size", params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
            DUMP_INTERVAL("    Periods", params, SNDRV_PCM_HW_PARAM_PERIODS);
#undef DUMP_INTERVAL
	} else {
	    log_info("refine failed");
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
	persz_min = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
	persz_max = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max;
	periods_min = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIODS)->min;	
	periods_max = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIODS)->max;	

	for(i = periods_min; i <= periods_max; i += 2)
	    if(persz_max * i >= ctx->block_max && persz_min * i <= ctx->block_max) break; 	
	
	if(i > periods_max || persz_max * i < ctx->block_max
		|| persz_min * i > ctx->block_max) {
	    log_err("failed to find proper period setup");
	    ret = LIBLOSSLESS_ERR_AU_SETCONF;
	    goto err_exit;
	}
	priv->periods = i;
	priv->period_size = ctx->block_max / i;

	log_info("selecting period size %d, periods %d", priv->period_size, priv->periods);

	param_set_int(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, priv->period_size);
	param_set_int(params, SNDRV_PCM_HW_PARAM_PERIODS, priv->periods);

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, params) < 0) {
	    log_err("cannot set hw parameters");
	    ret	= LIBLOSSLESS_ERR_AU_SETCONF;
	    goto err_exit;	
	}
	priv->setup_info = params->info;
	log_info("pcm hw setup succeeded: 0x%08X", params->info);

	k = priv->period_size * priv->periods * ctx->channels * pfmt->phys_bits/8;
	priv->pcm_buf = malloc(k);
	if(!priv->pcm_buf) {
	    log_err("no memory for buffer");
	    ret = LIBLOSSLESS_ERR_NOMEM;
	    goto err_exit;	
	}
        memset(priv->pcm_buf, 0, k);

	// TODOOOO!!!
#if 0
    struct snd_pcm_sw_params sparams;
	memset(&sparams, 0, sizeof(sparams));
	sparams.avail_min = 1;
	sparams.start_threshold = priv->period_size;
	sparams.stop_threshold = priv->period_size * priv->periods;
	sparams.xfer_align = priv->period_size / 2;
	sparams.silence_size = 0;
	sparams.silence_threshold = config->silence_threshold;
	sparams.boundary = priv->period_size * ctx->bps/8;
#endif
	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            log_err("prepare() failed");
            ret = LIBLOSSLESS_ERR_AU_SETCONF;
            goto err_exit;
        }
	for(k = 0; k < priv->periods; k++) {
	    int n = priv->period_size * ctx->channels * pfmt->phys_bits/8;
	    i = write(priv->pcm_fd, priv->pcm_buf, n);
	    if(i != n) {
		log_err("cannot fill initial buffer");
		ret = LIBLOSSLESS_ERR_AU_BUFF;
		goto err_exit;
	    }
	}
	log_info("setup complete");
	free(params);
	return 0;

    err_exit:	
	if(params) free(params);
	if(priv->pcm_buf) {
	    free(priv->pcm_buf);
	    priv->pcm_buf = 0;	
	}
	if(priv->pcm_fd >= 0) close(priv->pcm_fd);
	priv->pcm_fd = -1;
	log_err("exiting on error");

    return ret;
}

ssize_t alsa_write(playback_ctx *ctx, const void *buf, size_t count) 
{
    struct alsa_priv *priv;
    const struct playback_format_t *pfmt;
    int32_t **buffers, *src, *dst32;
    int16_t *dst16;
    int8_t  *dst8; 	
    int i, k, written;
    struct snd_xferi xf;

	if(!ctx || !ctx->apriv || ctx->apriv->pcm_fd < 0) {
	    log_err("invalid parameters");	
	    return 0;
	}
	priv = ctx->apriv;	
	pfmt = &supp_formats[priv->fmt_idx];

	if(count > priv->period_size * priv->periods) {
	    log_err("size %d exceeds buffer size %d", count, priv->period_size * priv->periods);
	    return 0;	
	}

	buffers = (int32_t **) buf;
	// memset(priv->pcm_buf, 0, priv->period_size * priv->periods * ctx->channels * pfmt->phys_bits/8);

	switch(pfmt->fmt) {
	    case SNDRV_PCM_FORMAT_S16_LE:
		for(i = 0; i < ctx->channels; i++) {
		    src = buffers[i];
		    dst16 = ((int16_t *) priv->pcm_buf) + i;	
		    for(k = 0; k < count; k++) {
			*dst16 = (int16_t) *src++;
		 	dst16 += ctx->channels;
		    }
		}
		break;
	    case SNDRV_PCM_FORMAT_S24_3LE:
		for(i = 0; i < ctx->channels; i++) {
		    src = buffers[i];
		    dst8 = ((int8_t *) priv->pcm_buf) + 3*i;	
		    for(k = 0; k < count; k++) {
			int32_t val = *src++;
			dst8[0] = val;
			dst8[1] = val >> 8;
			dst8[2] = val >> 16;
		 	dst8 += ctx->channels * 3;
		    }	
		}
		break;
	    case SNDRV_PCM_FORMAT_S24_LE:
	    case SNDRV_PCM_FORMAT_S32_LE:
		for(i = 0; i < ctx->channels; i++) {
		    src = buffers[i];
		    dst32 = ((int32_t *) priv->pcm_buf) + i;	
		    for(k = 0; k < count; k++) {
			*dst32 = *src++;
			dst32 += ctx->channels;
		    }	
		}
		break;
	    default:
		log_err("internal error: invalid format");
		return 0;
	}

#if 0
	xf.buf = priv->pcm_buf;
	xf.frames = count;
	written = 0;

	while(written < count) {
	    i = ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xf);
	    log_info("wrote %ld", xf.result);	
	    if(i != 0) {
		switch(errno) {
		   case EINTR:
		   case EAGAIN:
			usleep(1000);
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
	    }
	    written += xf.result;
	}	
#endif

#if 0
	for(k = 0, written = 0; k < priv->periods; k++) {
	    xf.buf = priv->pcm_buf + k * priv->period_size * ctx->channels * pfmt->phys_bits/8;
	    xf.frames = priv->period_size;
	    while(written < count) {
		i = ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xf);
		if(i != 0) {
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
		} else if(xf.result != priv->period_size) {
		    log_err("i=%d result=%ld!", i, xf.result);		
		    return 0;	
		} else written += xf.result;
	    }
	}
#endif
	for(k = 0, written = 0; k < priv->periods; k++) {
	    while(written < count) {
		i = write(priv->pcm_fd, priv->pcm_buf + k * priv->period_size * ctx->channels * pfmt->phys_bits/8,
			priv->period_size * ctx->channels * pfmt->phys_bits/8);
		if(i != priv->period_size * ctx->channels * pfmt->phys_bits/8) {
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
		} else written += priv->period_size;
	    }
	}

    return written;
}


void alsa_stop(playback_ctx *ctx) 
{
    struct alsa_priv *priv;	
	log_info("context %p priv %p", ctx, ctx ? ctx->apriv : 0);
	if(!ctx || !ctx->apriv) return;
	priv = ctx->apriv;
	if(priv->pcm_fd >= 0) close(priv->pcm_fd);
	priv->pcm_fd = -1;
	if(priv->pcm_buf) free(priv->pcm_buf);
	priv->pcm_buf = 0;
}

bool alsa_pause(playback_ctx *ctx) 
{
    if((ctx->apriv->setup_info & SNDRV_PCM_INFO_PAUSE) == 0) {
	log_err("cannot pause this stream");
	return false;
    }		
    // assume SND_PAUSE supported. if not, stop. buffer+byte counter should persist. 
    return false;
}

bool alsa_resume(playback_ctx *ctx) 
{
    return false;
}

bool alsa_set_volume(playback_ctx *ctx, int vol) 
{
    return false;
}

