# amznaec_shim for CM14.1 (Android 7.1)
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libamznaec_shim
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := 32

LOCAL_SRC_FILES := \
    amznaec_shim.cpp \
    speexdsp/libspeexdsp/mdf.c \
    speexdsp/libspeexdsp/preprocess.c \
    speexdsp/libspeexdsp/filterbank.c \
    speexdsp/libspeexdsp/fftwrap.c \
    speexdsp/libspeexdsp/kiss_fft.c \
    speexdsp/libspeexdsp/kiss_fftr.c

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/speexdsp/include \
    $(LOCAL_PATH)/speexdsp/libspeexdsp \
    external/webrtc \
    external/tinyalsa/include

LOCAL_SHARED_LIBRARIES := \
    libwebrtc_audio_preprocessing \
    libcutils \
    liblog \
    libdl

LOCAL_CFLAGS := \
    -DWEBRTC_POSIX \
    -DFLOATING_POINT \
    -DUSE_KISS_FFT \
    -DEXPORT= \
    -DVAR_ARRAYS \
    -Wall \
    -Wno-unused-parameter \
    -Wno-unused-variable \
    -Wno-unused-function \
    -Wno-sign-compare \
    -Wno-implicit-fallthrough

LOCAL_CPPFLAGS := -std=gnu++11

include $(BUILD_SHARED_LIBRARY)
