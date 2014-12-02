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
//#include <android/log.h>
#include <errno.h>

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>

#include "main.h"

/*
struct mixer_elem {
    int id;
    snd_ctl_elem_type_t et;
    char *name;
};*/

struct alsa_priv {
    int  card, device;
    int  pcm_fd, ctl_fd;
    struct snd_ctl_elem_info *controls;
    int supp_formats_mask;
    int supp_rates_mask;
    int setup_info;
    int msm_mm_idx;     /* X in MultiMediaX for msm devices */
    int fmt_idx;        /* index into struct supp_formats */
    int periods;
    int period_size;
    void *pcm_buf;
};


#define log_info(fmt,args...) printf(fmt "\n", ##args)
#define log_err(fmt,args...) printf(fmt "\n", ##args)
void alsa_close(void *ctx) {}

int init_mixer(int card, struct alsa_priv *priv, void *ctx) 
{
    int k;
    struct snd_ctl_elem_list elist;
    struct snd_ctl_elem_id *eid;

	memset(&elist, 0, sizeof elist);
	log_info("trying to open mixer");
	sprintf(tmp, "/dev/snd/controlC%d", card);
	priv->ctl_fd = open(tmp, O_RDWR);

	if(priv->ctl_fd < 0) {
	    log_err("cannot open %s", tmp);
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_SETUP;
	}
	if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
	    log_err("cannot get number of controls");
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_GETCONF;
	}
	log_info("mixer opened, control count: %d", elist.count);
	eid = calloc(elist.count, sizeof(struct snd_ctl_elem_id));
	if(eid) {
	    log_err("no memory\n");
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_NOMEM;
	}
	elist.space = elist.count;
	elist.pids = eid;
	if(ioctl(priv->ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
	    log_err("cannot get control ids");
	    alsa_close(ctx);
	    return LIBLOSSLESS_ERR_AU_GETCONF;
	}
	priv->controls = (struct snd_ctl_elem_info *) calloc(elist.count + 1, sizeof((struct snd_ctl_elem_info)));
	for(k = 0; k < elist.count; k++) {

	}	
    return 0;	
}


