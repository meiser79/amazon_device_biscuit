LOCAL_PATH := $(call my-dir)

# Headless HWC 1.1 for Biscuit. Selected with ro.hardware.hwcomposer=biscuit
# (see device.mk); the stock hwcomposer.mt8163.so stays installed as fallback.
include $(CLEAR_VARS)
LOCAL_MODULE := hwcomposer.biscuit
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := hwcomposer.cpp
LOCAL_CPPFLAGS := -std=gnu++11 -Wall -Werror
LOCAL_SHARED_LIBRARIES := liblog libcutils
include $(BUILD_SHARED_LIBRARY)
