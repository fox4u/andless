#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <android/log.h>
#include <FLAC/stream_decoder.h>
#include "main.h"

static void update_track_time(JNIEnv *env, jobject obj, int time); 

static jboolean audio_stop(playback_ctx *ctx) 
{
    if(!ctx) {
	log_err("no context to stop");
	return false;
    }	
    log_info("stopping context %p, state %d", ctx, ctx->state);
    pthread_mutex_lock(&ctx->mutex);	    
    if(ctx->state == STATE_STOPPED) {
    	pthread_mutex_unlock(&ctx->mutex);
	return false;
    }
    if(ctx->state == STATE_PAUSED) {
    	ctx->state = STATE_STOPPED;	
	pthread_mutex_unlock(&ctx->pause_mutex);
    } else ctx->state = STATE_STOPPED;	
    alsa_stop(ctx);
    pthread_mutex_unlock(&ctx->mutex);
    log_info("done");
    return true;	
}

static jboolean audio_pause(JNIEnv *env, jobject obj, playback_ctx *ctx) 
{
    bool ret;
    if(!ctx) {
	log_err("no context to pause");
	return false;
    }	
    pthread_mutex_lock(&ctx->mutex);
    if(ctx->state != STATE_PLAYING) {
    	pthread_mutex_unlock(&ctx->mutex);
	return false;
    }	
    ret = alsa_pause(ctx);
    if(ret) {	
	ctx->state = STATE_PAUSED;
	pthread_mutex_lock(&ctx->pause_mutex);
    }	
    pthread_mutex_unlock(&ctx->mutex);
    return ret;		
}

static jboolean audio_resume(JNIEnv *env, jobject obj, playback_ctx *ctx) 
{
    bool ret;
    if(!ctx) {
	log_err("no context to resume");
	return false;
    }	
    pthread_mutex_lock(&ctx->mutex);
    if(ctx->state != STATE_PAUSED) {
    	pthread_mutex_unlock(&ctx->mutex);
	return false;
    } 	
    ret = alsa_resume(ctx);	
    if(ret) log_err("resume failed, proceeding anyway");
    ctx->state = STATE_PLAYING;	
    pthread_mutex_unlock(&ctx->pause_mutex);
    pthread_mutex_unlock(&ctx->mutex);
    return ret;	
}

static jint audio_get_duration(JNIEnv *env, jobject obj, playback_ctx *ctx) 
{
   if(!ctx || (ctx->state != STATE_PLAYING && ctx->state != STATE_PAUSED)) return 0;	
   return ctx->track_time;
}

/* in seconds */
static jint audio_get_cur_position(JNIEnv *env, jobject obj, playback_ctx *ctx) 
{
   if(!ctx || (ctx->state != STATE_PLAYING && ctx->state != STATE_PAUSED)) return 0;
   return ctx->written/ctx->samplerate;
}

static jint audio_init(JNIEnv *env, jobject obj, playback_ctx *prev_ctx, jint card, jint device) 
{
    playback_ctx *ctx;

    log_info("audio_init: prev_ctx=%p", prev_ctx);
    if(prev_ctx) {
	audio_stop(prev_ctx);
	if(alsa_select_device(prev_ctx, card, device) != 0) { 
	    return 0;
	}
	ctx = prev_ctx;
    } else {
	ctx = (playback_ctx *) calloc(1, sizeof(playback_ctx));
	if(!ctx) return 0;
	if(alsa_select_device(ctx,card,device) != 0) {
	    free(ctx);	
	    return 0;	
	}
	pthread_mutex_init(&ctx->mutex,0);
	pthread_mutex_init(&ctx->pause_mutex,0);
    }
    ctx->track_time = 0;
    log_info("audio_init: return ctx=%p",ctx);
    return (jint) ctx;	
}

static jboolean audio_exit(JNIEnv *env, jobject obj, playback_ctx *ctx) 
{
    if(!ctx) {
	log_err("zero context");
	return false;
    }	
    log_info("audio_exit: ctx=%p",ctx);
    audio_stop(ctx);
    alsa_close(ctx);
    pthread_mutex_destroy(&ctx->mutex);
    pthread_mutex_destroy(&ctx->pause_mutex);
    if(ctx->apriv) free(ctx->apriv);	
    free(ctx);	
    return true;
}

static jboolean audio_set_volume(JNIEnv *env, jobject obj, playback_ctx *ctx, jint vol) 
{
    if(!ctx || ctx->state != STATE_PLAYING) {
	log_err("%s", ctx ? "invalid state" : "no context");
	return false;
    }
    pthread_mutex_lock(&ctx->mutex);
    alsa_set_volume(ctx, vol);	
    pthread_mutex_unlock(&ctx->mutex);
    return true;	
}

////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////

static void err_cb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *cbdata) {
    log_err("decoder error: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
}

static void metadata_cb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *cbdata) 
{
    playback_ctx *ctx = (playback_ctx *) cbdata;
	if(metadata->type != FLAC__METADATA_TYPE_STREAMINFO) return;
	log_info("streaminfo received");
	ctx->samplerate = metadata->data.stream_info.sample_rate;
	ctx->channels = metadata->data.stream_info.channels;	
	ctx->bps = metadata->data.stream_info.bits_per_sample;
	ctx->track_time = metadata->data.stream_info.total_samples * ctx->samplerate;
	ctx->block_min = metadata->data.stream_info.min_blocksize;
	ctx->block_max = metadata->data.stream_info.max_blocksize;
}

static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *decoder, 
	const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *cbdata) 
{
    int i = 0, frames = frame->header.blocksize;
    playback_ctx *ctx = (playback_ctx *) cbdata;
    //	log_info("write cb %d", frames);	
	pthread_mutex_lock(&ctx->mutex);	    
	if(ctx->state == STATE_PAUSED) {	   /* pause_mutex must be locked here */
	    pthread_mutex_unlock(&ctx->mutex);	
	    pthread_mutex_lock(&ctx->pause_mutex); /* stop until the state changes from PAUSED */
	    pthread_mutex_lock(&ctx->mutex);
	}
	if(ctx->state == STATE_STOPPED || (i = alsa_write(ctx, buffer, frames)) != frames) {
	    pthread_mutex_unlock(&ctx->mutex);
	    log_info("stopping from write callback: state %d written %d", ctx->state, i);
	    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	ctx->written += frames;
	pthread_mutex_unlock(&ctx->mutex);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}


static jint flac_play(JNIEnv *env, jobject obj, playback_ctx* ctx, jstring jfile, jint start) 
{
    int ret = 0;
    const char *file = (*env)->GetStringUTFChars(env,jfile,NULL);
    FLAC__StreamDecoder *decoder = 0;
    
	if(!ctx) {
	    log_err("no context");	
	    ret = LIBLOSSLESS_ERR_NOCTX;
	    goto done;	
	}
	if(!file) {
	    log_err("no file specified");	
	    ret = LIBLOSSLESS_ERR_INV_PARM;
	    goto done;
        }
	if((decoder = FLAC__stream_decoder_new()) == NULL) {
	    log_err("failed to allocate decoder\n");
	    ret = LIBLOSSLESS_ERR_DECODE;
	    goto done;
        }
	log_info("trying to play file %s, context %p", file, ctx);
	FLAC__stream_decoder_set_md5_checking(decoder,false);
	if(FLAC__stream_decoder_init_file(decoder, file, write_cb, metadata_cb, err_cb, ctx) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
	    log_err("error initializing decoder\n");
	    ret = LIBLOSSLESS_ERR_DECODE;
	    goto done;	
        }
	(*env)->ReleaseStringUTFChars(env,jfile,file);  
	file = 0;

	if(!FLAC__stream_decoder_process_until_end_of_metadata(decoder)) {
	    log_err("error processing flac metadata\n");
	    ret = LIBLOSSLESS_ERR_DECODE;
	    goto done;	
	}
	if(start) {
	    if(!FLAC__stream_decoder_seek_absolute(decoder, ((FLAC__uint64) start) * ctx->samplerate)) {
		ret = LIBLOSSLESS_ERR_OFFSET;
		goto done;
	    }	
	}
	log_info("starting alsa");

    	pthread_mutex_lock(&ctx->mutex);
	ret = alsa_start(ctx);
	if(ret == 0) ctx->state = STATE_PLAYING;
    	pthread_mutex_unlock(&ctx->mutex);

	if(ret) goto done;
	log_info("alsa started");

	update_track_time(env,obj,ctx->track_time);

	if(!FLAC__stream_decoder_process_until_end_of_stream(decoder)) ret = LIBLOSSLESS_ERR_DECODE;
	log_info("playback complete");

	if(!audio_stop(ctx)) ret = 0; /* we've been stopped manually */

    done:
	if(ret) log_err("exiting on error %d", ret);
	if(file) (*env)->ReleaseStringUTFChars(env,jfile,file);
	if(decoder) FLAC__stream_decoder_delete(decoder);
	return ret;

}

static jintArray *extract_flac_cue(JNIEnv *env, jobject obj, jstring jfile) {
    return 0;	
}

/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////

#ifndef CLASS_NAME
#error "CLASS_NAME not set in Android.mk"
#endif

static jboolean libinit(JNIEnv *env, jobject obj, jint sdk) 
{
    return true;
}

static jboolean libexit(JNIEnv *env, jobject obj) 
{
    return true;
}

static JavaVM *gvm;
static jobject giface; 

static void update_track_time(JNIEnv *env, jobject obj, int time) 
{
     jclass cls;
     jmethodID mid;
     bool attached = false;
     JNIEnv *envy;

	if((*gvm)->GetEnv(gvm, (void **)&envy, JNI_VERSION_1_4) != JNI_OK) {
            log_err("GetEnv FAILED");
	     if((*gvm)->AttachCurrentThread(gvm, &envy, NULL) != JNI_OK) {
            	log_err("AttachCurrentThread FAILED");
		     return;
	     }	
	     attached = true;	
	}
	cls = (*envy)->GetObjectClass(envy,giface);
	if(!cls) {
          log_err("failed to get class iface");
	  return;  	
	}
        mid = (*env)->GetStaticMethodID(envy, cls, "updateTrackLen", "(I)V");
        if(mid == NULL) {
	  log_err("Cannot find java callback to update time");
         return; 
        }
	(*envy)->CallStaticVoidMethod(envy,cls,mid,time);
	if(attached) (*gvm)->DetachCurrentThread(gvm);
}

static jboolean audio_stop_exp(JNIEnv *env, jobject obj, playback_ctx *ctx) 
{
    return audio_stop(ctx);    	
}


static JNINativeMethod methods[] = {
 { "audioInit", "(III)I", (void *) audio_init },
 { "audioExit", "(I)Z", (void *) audio_exit },
 { "audioStop", "(I)Z", (void *) audio_stop_exp },
 { "audioPause", "(I)Z", (void *) audio_pause },
 { "audioResume", "(I)Z", (void *) audio_resume },
 { "audioGetDuration", "(I)I", (void *) audio_get_duration },
 { "audioGetCurPosition", "(I)I", (void *) audio_get_cur_position },
 { "audioSetVolume", "(II)Z", (void *) audio_set_volume },
 { "flacPlay", "(ILjava/lang/String;I)I", (void *) flac_play },
 { "extractFlacCUE", "(Ljava/lang/String;)[I", (void *) extract_flac_cue },
 { "libInit", "(I)Z", (void *) libinit },
 { "libExit", "()Z", (void *) libexit },
};

jint JNI_OnLoad(JavaVM* vm, void* reserved) 
{
    jclass clazz = NULL;
    JNIEnv* env = NULL;
    jmethodID constr = NULL;
    jobject obj = NULL;

      log_info("JNI_OnLoad");
      gvm = vm;	

      if((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_4) != JNI_OK) {
        log_err("GetEnv FAILED");
        return -1;
      }

      clazz = (*env)->FindClass(env, CLASS_NAME);
      if(!clazz) {
        log_err("Registration unable to find class '%s'", CLASS_NAME);
        return -1;
      }
      constr = (*env)->GetMethodID(env, clazz, "<init>", "()V");
      if(!constr) {
        log_err("Failed to get constructor");
	return -1;
      }
      obj = (*env)->NewObject(env, clazz, constr);
      if(!obj) {
        log_err("Failed to create an interface object");
	return -1;
      }
      giface = (*env)->NewGlobalRef(env,obj);

      if((*env)->RegisterNatives(env, clazz, methods, sizeof(methods) / sizeof(methods[0])) < 0) {
        log_err("Registration failed for '%s'", CLASS_NAME);
        return -1;
      }
    
   return JNI_VERSION_1_4;
}

