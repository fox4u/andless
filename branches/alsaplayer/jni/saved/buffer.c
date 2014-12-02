#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>


#define log_info(fmt, args...)  printf("%s: " fmt "\n", __func__, ##args)
#define log_err log_info

/* Pcm_buffer is a ring buffer used by the decoder (writer) to provide frames and by the player (reader) to obtain them.
   The decoder outputs frames in chunks of possibly variable size. The player reads frames in chunks of 
   a fixed "period_size" defined based on hardware capabilities and related latency requirements. 
   Each time the player tries to read, the frames should be available at once, otherwise an underrun may occur.
   The below implementation assumes a single reader and a single writer. */


typedef struct pcm_buffer_t {
    void *mem;
    int size;
    int start_idx;	/* index of first valid byte to read */
    int bytes;		/* bytes currently occupying buffer */
    volatile int should_run;
    volatile int abort;		/* terminate immediately */
    pthread_mutex_t mutex;
    pthread_mutex_t mutex_cond;
    pthread_cond_t  buff_changed;
} pcm_buffer;

pcm_buffer *buffer_create(int size) 
{
    pcm_buffer *buff = (pcm_buffer *) calloc(1, sizeof(pcm_buffer));
    if(!buff) return 0;	
    buff->size = size;	
    buff->mem = malloc(size);
    if(!buff->mem) {
	free(buff);
	return 0;
    }
    pthread_mutex_init(&buff->mutex, 0);	
    pthread_mutex_init(&buff->mutex_cond, 0);	
    pthread_cond_init(&buff->buff_changed, 0);
    buff->should_run = 1;
    return buff;
}

void buffer_destroy(pcm_buffer *buff)
{
    if(buff) {
	if(buff->mem) free(buff->mem);
	pthread_mutex_destroy(&buff->mutex);
	pthread_mutex_destroy(&buff->mutex_cond);
	pthread_cond_destroy(&buff->buff_changed);
    }	
}

static inline void signal_buffer_changed(pcm_buffer *buff) 
{
    pthread_mutex_lock(&buff->mutex_cond);
    pthread_cond_broadcast(&buff->buff_changed);
    pthread_mutex_unlock(&buff->mutex_cond);
}

void buffer_stop(pcm_buffer *buff)
{
    pthread_mutex_lock(&buff->mutex);
    buff->should_run = 0;
    pthread_mutex_unlock(&buff->mutex);
    signal_buffer_changed(buff);	
}

void buffer_abort(pcm_buffer *buff)
{
    pthread_mutex_lock(&buff->mutex);
    buff->abort = 1;	
    pthread_mutex_unlock(&buff->mutex);
    signal_buffer_changed(buff);	
}

/* Returns negative on error, zero on stop or abort, 
   otherwise the same nubmer of bytes as requested */

int put_frames(pcm_buffer *buff, void *src, int bytes) 
{
    int end_idx, k;

    if(bytes <= 0 || bytes > buff->size) return -1;

    pthread_mutex_lock(&buff->mutex);
    log_info("entry");

    while(buff->size - buff->bytes <  bytes && buff->should_run && !buff->abort) {
	k = buff->size;
	pthread_mutex_unlock(&buff->mutex);
    	log_info("wait: bufsz %d", buff->bytes);
	pthread_mutex_lock(&buff->mutex_cond);
	pthread_cond_signal(&buff->buff_changed);
	pthread_cond_wait(&buff->buff_changed, &buff->mutex_cond);
	pthread_mutex_unlock(&buff->mutex_cond);
	pthread_mutex_lock(&buff->mutex);
    }	
    if(!buff->should_run || buff->abort) {
    	pthread_mutex_unlock(&buff->mutex);	  
    	log_info("abort");	
	return 0;	
    }
    log_info("putting");
    end_idx = buff->start_idx + buff->bytes;
    if(end_idx >= buff->size) end_idx -= buff->size;
    if(end_idx + bytes > buff->size) {
	k = buff->size - end_idx;
	memcpy(buff->mem + end_idx, src, k);
	memcpy(buff->mem, src + k, bytes - k);
    } else memcpy(buff->mem + end_idx, src, bytes);
    buff->bytes += bytes;	    	

    log_info("put %d bufsz %d start %d", bytes, buff->bytes, buff->start_idx);	
    pthread_mutex_unlock(&buff->mutex);	  
    log_info("signalling");
    signal_buffer_changed(buff);	
    log_info("done");
     		
    return bytes;  
}

/* Returns negative on error, zero on abort, 
   or number of bytes that may be less than requested 
   if should_run = 0 */

int get_frames(pcm_buffer *buff, void *dst, int bytes)
{
    int k;     	

    if(bytes <= 0 || bytes > buff->size) return -1;

    pthread_mutex_lock(&buff->mutex);
    log_info("entry");	

    while(bytes > buff->bytes && buff->should_run && !buff->abort) {
	k = buff->size;
	pthread_mutex_unlock(&buff->mutex);
    	log_info("wait: bufsz %d", buff->bytes);	
	pthread_mutex_lock(&buff->mutex_cond);
	pthread_cond_signal(&buff->buff_changed);
	pthread_cond_wait(&buff->buff_changed, &buff->mutex_cond);
	pthread_mutex_unlock(&buff->mutex_cond);
	pthread_mutex_lock(&buff->mutex);
	if(!buff->should_run) break;
    }	
    if(buff->abort) {
    	pthread_mutex_unlock(&buff->mutex);	  
    	log_info("abort");	
	return 0;	
    }
    if(bytes > buff->bytes) bytes = buff->bytes; /* end of stream */
    log_info("geting");
    k = buff->size - buff->start_idx;
    if(k < bytes) {
	memcpy(dst, buff->mem + buff->start_idx, k);
	memcpy(dst + k, buff->mem, bytes - k);
	buff->start_idx = bytes - k;
    } else {
	memcpy(dst, buff->mem + buff->start_idx, bytes);
	buff->start_idx += bytes;
	if(buff->start_idx == buff->size) buff->start_idx = 0;
    }	
    buff->bytes -= bytes;	
    log_info("got %d bufsz %d start %d", bytes, buff->bytes, buff->start_idx);	
    pthread_mutex_unlock(&buff->mutex);	  
    log_info("signalling");
    signal_buffer_changed(buff); 	
    log_info("done");

    return bytes;
}

struct thd {
    pcm_buffer *buff;
    int size;
};

void *reader(void *ttt) {
    int k;
    struct thd *t = (struct thd *) ttt;
    char *c = malloc(t->size);

#if 0
    struct sched_param sparam;
	sparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sparam);
#endif
	while(1) {	
	    k = get_frames(t->buff, c, t->size);
	    if(!k) {
		log_info("abort");
		break;
	    }
	    if(k < t->size) {
		log_info("short read %d", k);
		break;
	    }
	}
}

#define RD_SZ  ( 1024 )
#define WR_SZ  ( 4096 )
#define BSZ	( 128 * 1024 )

int main(int argc, char **argv) {

    pcm_buffer *buff;
    char *c;
    int cycles;
    pthread_t pth;
    struct thd tt;

	if(argc < 2) return log_err("no args specified");
	cycles = atoi(argv[1]);	
	   
	buff = buffer_create(BSZ);
	if(!buff) return log_err("cannot init");

	tt.buff = buff;
	tt.size = RD_SZ;
	c = malloc(WR_SZ);

	if(pthread_create(&pth, 0, reader, &tt) != 0) return log_err("cannot create thread");
	log_info("running %d cycles", cycles);

	while(cycles) {
	     int k;
		log_info("cycle %d", cycles);
		k = put_frames(buff, c, WR_SZ);	
		if(k != WR_SZ) {
		    log_err("writer error %d", k);
		    break;	
		}	
	     cycles--;	
	}
	log_info("stopping buffer");
	buffer_stop(buff);
	usleep(5);
	buffer_stop(buff);
	log_info("joining");
	pthread_join(pth, 0);
	log_info("joined");	
	buffer_destroy(buff);
    return 0;	

}


