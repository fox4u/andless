
LOCAL_PATH := $(call my-dir)

# common codecs & startup library
include $(CLEAR_VARS)
LOCAL_MODULE := flac
LOCAL_CFLAGS += -O3 -funroll-loops -Wall -finline-functions -fPIC -DPIC
LOCAL_CFLAGS += -DHAVE_CONFIG_H -Iinclude -D_REENTRANT -DFLAC__NO_MD5 # -DFLAC__INTEGER_ONLY_LIBRARY
LOCAL_SRC_FILES := bitmath.c  bitreader.c  bitwriter.c  cpu.c  crc.c  fixed.c  float.c  format.c  lpc.c  md5.c  memory.c  stream_decoder.c 
LOCAL_ARM_MODE := arm
include $(BUILD_STATIC_LIBRARY)


