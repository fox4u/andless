#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>


#define MAX_CARDS	32

struct card_info {
    char *card_name;
    char *card_id;
    int  pcm_num;
    char **pcm_names;	
    int  *pcm_ids;
};


struct card_info *alsa_get_cards() {
    char **names, tmp[128];
    int *ids, fd, k, i, pcms;
    struct card_info *cards = 0;     
    struct snd_ctl_card_info info;
    struct snd_pcm_info pcm_info;

	cards = (struct card_info *) calloc(MAX_CARDS, sizeof(struct card_info));
        if(!cards) return 0;

        for(k = 0; k < MAX_CARDS; k++) {
            sprintf(tmp, "/dev/snd/controlC%d", k);     
            fd = open(tmp, O_RDWR);  
            if(fd < 0) break;
            if(ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) != 0) break;
	    cards[k].card_name = strdup(info.name);
	    cards[k].card_id = strdup(info.id);
	    for(pcms = 0, names = 0, ids = 0, i = -1; ; pcms++) {
		if(ioctl(fd, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE, &i) || i == -1) break;
	        memset(&pcm_info, 0, sizeof(struct snd_pcm_info));
		pcm_info.device = i;
		if(ioctl(fd, SNDRV_CTL_IOCTL_PCM_INFO, &pcm_info) == 0) {
		    names = realloc(names, sizeof(char *) * (pcms+1));
		    ids = realloc(ids, sizeof(int) * (pcms+1));
		    names[pcms]	= strdup(pcm_info.id);
		    ids[pcms] = i;
		}
	    } 
	    cards[k].pcm_num = pcms;	
	    cards[k].pcm_names = names;  	
	    cards[k].pcm_ids = ids;  	
	    close(fd);	
        }
    return cards;       
}

int main() {
    int i, k;
    struct card_info *cards = alsa_get_cards();
   	for(i = 0; i < MAX_CARDS; i++) {
	    if(!cards[i].card_name) break;	
	    printf("Card %s [%s]:\n", cards[i].card_name, cards[i].card_id);	
	    for(k = 0; k < cards[i].pcm_num; k++)
		printf("hw:%d,%d\t%s\n", i, cards[i].pcm_ids[k], cards[i].pcm_names[k]);
	}

}

