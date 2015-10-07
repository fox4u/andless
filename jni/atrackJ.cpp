/*
 * atrackJ.cpp
 *
 *  Created on: 2015-10-7
 *      Author: fox
 */

#include <jni.h>
#include <android/log.h>
#include <sys/system_properties.h>


#include "main.h"
#define FROM_ATRACK_CODE 1
#include "std_audio.h"

#define LOG_TAG "atrackJ"
#define LOGD(f,v)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,f,v)

extern "C" {

static jclass audio_track_cls = NULL;
static jmethodID method_stop = NULL;
static jmethodID method_flush = NULL;
static jmethodID method_play = NULL;
static jmethodID method_pause = NULL;
static jmethodID method_write = NULL;
static jbyteArray buffer = NULL;

JNIEnv* check_jni_env()
{
    JNIEnv* env = NULL;
    JavaVM* jvm = getJVM();
    jint ret = jvm->GetEnv((void**)&env, JNI_VERSION_1_4);
    switch (ret) {
    case JNI_OK:
        break;

    case JNI_EDETACHED :
        if (jvm->AttachCurrentThread(&env, NULL) < 0)
        {
            LOGD("%s", "Failed to get the environment using AttachCurrentThread()");
        }
        break;

    case JNI_EVERSION :
    default :
        LOGD("%s", "Failed to get the environment using GetEnv()");
        break;
    }

    if(!audio_track_cls)
    {
        audio_track_cls = reinterpret_cast<jclass>(env->NewGlobalRef(env->FindClass("android/media/AudioTrack")));
        if(!audio_track_cls)
        {
            LOGD("%s", "unable to get class of AudioTrack");
            env = NULL;
        }
        else
        {
            method_stop = env->GetMethodID(audio_track_cls, "stop", "()V");
            method_flush = env->GetMethodID(audio_track_cls, "flush", "()V");
            method_play = env->GetMethodID(audio_track_cls, "play", "()V");
            method_pause = env->GetMethodID(audio_track_cls, "pause", "()V");
            method_write = env->GetMethodID(audio_track_cls, "write", "([BII)I");

            if(!method_stop || !method_flush || !method_play || !method_pause || !method_write)
            {
                LOGD("%s", "unable to get method of AudioTrack");
                env->DeleteGlobalRef(audio_track_cls);
                audio_track_cls = NULL;
                env = NULL;
            }
        }
    }

    return env;
}


int libmedia_start(msm_ctx *ctx, int channels, int samplerate) {

    JNIEnv *env = check_jni_env();
    if(!env) return LIBLOSSLESS_ERR_NOCTX;

    if(!ctx) return LIBLOSSLESS_ERR_NOCTX;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "libmedia_start chans=%d rate=%d afd=%d atrack=%p",
            channels, samplerate,ctx->afd,ctx->track);

    if(ctx->track && ctx->samplerate == samplerate && ctx->channels == channels) {
        jobject audio_track = (jobject)ctx->track;
        env->CallVoidMethod(audio_track, method_stop);
        env->CallVoidMethod(audio_track, method_flush);
        env->CallVoidMethod(audio_track, method_play);
        return 0;
    }

    if(ctx->track) {
        jobject audio_track = (jobject)ctx->track;
        env->CallVoidMethod(audio_track, method_stop);
        env->CallVoidMethod(audio_track, method_flush);

        env->DeleteGlobalRef(audio_track);
    }

    int chans = (channels == 2) ? 12 : 4; // CHANNEL_OUT_STEREO / CHANNEL_OUT_MONO
    jmethodID constructor_id = env->GetMethodID(audio_track_cls, "<init>",
            "(IIIIII)V");
    jobject audio_track = env->NewObject(audio_track_cls,
            constructor_id,
            3,            /*AudioManager.STREAM_MUSIC*/
            samplerate,   /*sampleRateInHz*/
            chans,
            2,            /*ENCODING_PCM_16BIT*/
            DEFAULT_CONF_BUFSZ/(2*channels),  /*bufferSizeInBytes*/
            1             /*AudioTrack.MODE_STREAM*/
    );

    ctx->track = reinterpret_cast<jclass>(env->NewGlobalRef(audio_track));
    __android_log_print(ANDROID_LOG_INFO,LOG_TAG,"AudioTrack setup OK, starting audio!");
    ctx->conf_size = DEFAULT_CONF_BUFSZ;

    env->CallVoidMethod(audio_track, method_play);
    __android_log_print(ANDROID_LOG_INFO,LOG_TAG,"playback started!");

    if(!buffer)
    {
        buffer = reinterpret_cast<jbyteArray>(env->NewGlobalRef(env->NewByteArray(DEFAULT_CONF_BUFSZ)));
    }

    return 0;
}

void libmedia_stop(msm_ctx *ctx)
{
    JNIEnv *env = check_jni_env();
    if(!env) return;

    __android_log_print(ANDROID_LOG_INFO,LOG_TAG,"libmedia_stop called, ctx=%p, track=%p", ctx,
            ctx ? ctx->track : 0);

    if(ctx && ctx->track) {
        jobject audio_track = (jobject)ctx->track;
        env->CallVoidMethod(audio_track, method_pause);

        ctx->track_time = 0;
        ctx->state = (msm_ctx::_msm_state_t) 0;

    }
}

void libmedia_pause(msm_ctx *ctx) {
    JNIEnv *env = check_jni_env();
    if(!env) return;

    if(ctx && ctx->track) {
        jobject audio_track = (jobject)ctx->track;
        env->CallVoidMethod(audio_track, method_pause);
    }
}

void libmedia_resume(msm_ctx *ctx) {
    JNIEnv *env = check_jni_env();
    if(!env) return;

    if(ctx && ctx->track) {
        jobject audio_track = (jobject)ctx->track;
        env->CallVoidMethod(audio_track, method_play);
    }
}

ssize_t libmedia_write(msm_ctx *ctx, const void *buf, size_t count) {
//  if(ctx && ctx->track) return ((AudioTrack *) ctx->track)->write(buf, count);
//  else return -1;

	int ret = -1;
    JNIEnv *env = check_jni_env();
    if(!env) return ret;

    if(ctx && ctx->track) {
        jobject audio_track = (jobject)ctx->track;
        env->SetByteArrayRegion(buffer, 0, count, (jbyte *)buf);
        ret = env->CallIntMethod(audio_track, method_write,buffer, 0, count);
    }

    return ret;
}
}

