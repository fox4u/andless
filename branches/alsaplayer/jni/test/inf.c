#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>

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

static uint32_t test_rates[] = { 5512, 8000, 11025, 16000, 22050, 
	32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 0 };

static struct { int id; char *name; } test_formats[] = {
	{ 0, "S8" }, { 1, "U8" }, { 2, "S16_LE" }, { 3, "S16_BE" }, { 4, "U16_LE" }, { 5, "U16_BE" }, 
	{ 6, "S24_LE" }, { 7, "S24_BE" }, { 8, "U24_LE" }, { 9, "U24_BE" }, { 10, "S32_LE" }, 
	{ 11, "S32_BE" }, { 12, "U32_LE" }, { 13, "U32_BE" }, { 14, "FLOAT_LE" }, { 15, "FLOAT_BE" }, 
	{ 16, "FLOAT64_LE" }, { 17, "FLOAT64_BE" }, { 18, "IEC958_SUBFRAME_LE" }, { 19, "IEC958_SUBFRAME_BE" }, 
	{ 20, "MU_LAW" }, { 21, "A_LAW" }, { 22, "IMA_ADPCM" }, { 23, "MPEG" }, { 24, "GSM" }, 
	{ 31, "SPECIAL" }, { 32, "S24_3LE" }, { 33, "S24_3BE" }, { 34, "U24_3LE" }, { 35, "U24_3BE" }, 
	{ 36, "S20_3LE" }, { 37, "S20_3BE" }, { 38, "U20_3LE" }, { 39, "U20_3BE" }, { 40, "S18_3LE" }, 
	{ 41, "S18_3BE" }, { 42, "U18_3LE" }, { 43, "U18_3BE" }, { 44, "G723_24" }, { 45, "G723_24_1B" }, 
	{ 46, "G723_40" }, { 47, "G723_40_1B" }, { 48, "DSD_U8" }, { 49, "DSD_U16_LE" }, { 0, 0 } };

static void param_init(struct snd_pcm_hw_params *p);

int main(int argc, char **argv) 
{
    int k, fd = -1, fdm = -1, card = 0, device = 0, capture = 0, verbose = 0;
    char fn[256], *init_ctl = 0;
    struct snd_pcm_info info;
    struct snd_pcm_hw_params *params;

    struct snd_ctl_elem_list elist;
    struct snd_ctl_elem_id *eid = 0;
    struct snd_ctl_elem_info ei;
    struct snd_ctl_elem_value ev;

    void usage() 
    {
	printf("Usage: %s [-c card#] [-d device#] [-C] [-v] [-i init_mixer_ctl]\n", argv[0]);
	printf("-C = capture (default playback), -v = verbose, -i => control required to open device.\n");
	if(init_ctl) free(init_ctl);
	exit(1);
    }	

	while((k = getopt(argc, argv, "Cvc:d:i:")) != -1) {
	    switch(k) {
		case 'c': card = atoi(optarg); break;	
		case 'd': device = atoi(optarg); break;	
		case 'i': init_ctl = strdup(optarg); break;	
		case 'C': capture = 1; break;	
		case 'v': verbose = 1; break;
		default:  usage();	/* bad flag */
	    }	
	}

	if(optind != argc) usage();	/* extra args */

	if(init_ctl) {
		snprintf(fn, sizeof(fn), "/dev/snd/controlC%u", card);
		fdm = open(fn, O_RDWR);	
		if(fdm < 0) {
		    printf("warning: cannot open mixer\n");
		    goto done;	
		}
		memset(&elist, 0, sizeof(elist));
		memset(&ei, 0, sizeof(ei));

		/* get number of controls */
		if(ioctl(fdm, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) goto err_done;
	
		eid = calloc(elist.count, sizeof(struct snd_ctl_elem_id));
		if(!eid) goto err_done;		

		elist.space = elist.count;
		elist.pids = eid;
		/* get control ids */
		if(ioctl(fdm, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) goto err_done;

		for(k = 0; k < elist.count; k++) {

		    /* get info for each control id */
		    ei.id.numid = eid[k].numid;
		    if(ioctl(fdm, SNDRV_CTL_IOCTL_ELEM_INFO, &ei) < 0) goto err_done;
		    
		    if(strcmp(ei.id.name, init_ctl) == 0) {
			/* it's our control, get/set its vaule */
			memset(&ev, 0, sizeof(ev));
			ev.id.numid = ei.id.numid;
			if(ioctl(fdm, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) goto err_done;

			ev.value.integer.value[0] = 1;
			if(ioctl(fdm, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) goto err_done;

			if(verbose) printf("==> Mixer control found and set to 1\n");
			goto done;
		    } 
		}
	    err_done:
		close(fdm);
		fdm = -1;
		printf("warning: failed to set mixer control\n");
	    done:
		if(eid) free(eid);
		free(init_ctl);	
	}

	snprintf(fn, sizeof(fn), "/dev/snd/pcmC%uD%u%c", card, device, capture ? 'c' : 'p');

	fd = open(fn, O_RDONLY);
	if (fd < 0) {
	    printf("failed to open %s\n", fn);
	    goto err_open;
	}

	if (ioctl(fd, SNDRV_PCM_IOCTL_INFO, &info)) {
	    printf("cannot get info");
	    goto fail_close;
	}
	printf("device=hw:%d,%d stream=%s id=%s\n", 
		info.card, info.device, info.stream ? "capture" : "playback", info.id);
	params = calloc(1, sizeof(struct snd_pcm_hw_params));
	if (!params) goto fail_close;

	param_init(params);

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, params)) {
	    printf("SNDRV_PCM_IOCTL_HW_REFINE failed\n"); 
	    goto err_hw_refine;
	}
	printf("Device info=%08X\n", params->info);

#define DUMP_INTERVAL(NAME, PARAMS, IVAL)	do {	\
   printf(NAME ": min=%d\tmax=%d\n", param_to_interval(PARAMS, IVAL)->min, param_to_interval(PARAMS, IVAL)->max);	\
	} while(0)
	
	DUMP_INTERVAL("Sample bits", params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
	DUMP_INTERVAL("   Channels", params, SNDRV_PCM_HW_PARAM_CHANNELS);
	DUMP_INTERVAL("Period size", params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	DUMP_INTERVAL("    Periods", params, SNDRV_PCM_HW_PARAM_PERIODS);
	close(fd);
#undef DUMP_INTERVAL

	printf("Supported formats:");
	for(k = 0; test_formats[k].name; k++) {
	    fd = open(fn, O_RDWR);
	    param_init(params);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, test_formats[k].id);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	    if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, params) == 0) printf(" %s", test_formats[k].name);
	    close(fd);
	}
	printf("\n");

	printf("Supported samplerates:");
	for(k = 0; test_rates[k]; k++) {
	    fd = open(fn, O_RDWR);
	    param_init(params);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, test_rates[k]);
	    if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, params) == 0) printf(" %d", test_rates[k]);
	    close(fd);
	}
	printf("\n");
	k = 0;
	goto all_done;

    err_hw_refine:
	free(params);
    fail_close:
	close(fd);
    err_open:
	k = 1;
    all_done:
	if(fdm >= 0) {
	    ev.value.integer.value[0] = 0;
	    if(ioctl(fdm, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) printf("failed to reset mixer control\n");
	    else if(verbose) printf("==> Mixer control reset to 0\n");	
	    close(fdm);
	}
	return k;	
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



