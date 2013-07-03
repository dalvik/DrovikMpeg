LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := avfilter
LOCAL_SRC_FILES := libavfilter.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avutil
LOCAL_SRC_FILES := libavutil.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avcodec
LOCAL_SRC_FILES := libavcodec.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avdevice
LOCAL_SRC_FILES := libavdevice.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := swscale
LOCAL_SRC_FILES := libswscale.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := swresample
LOCAL_SRC_FILES := libswresample.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avformat
LOCAL_SRC_FILES := libavformat.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS -Wno-sign-compare -Wno-switch -Wno-pointer-sign -DHAVE_NEON=1  -mfpu=neon -mfloat-abi=softfp -fPIC -DANDROID

LOCAL_C_INCLUDES += \
	$(JNI_H_INCLUDE)
	
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include \

LOCAL_SRC_FILES := 	\
	player.c \
	register_natives.c

#LOCAL_STATIC_LIBRARIES :=avfilter avformat avcodec avdevice avutil swscale swresample

LOCAL_LDLIBS :=-L$(NDK_PLATFORMS_ROOT)/$(TARGET_PLATFORM)/arch-arm/usr/lib \
-L$(LOCAL_PATH) \
-lavfilter -lavformat -lavcodec -lavdevice -lavutil -lswscale -lswresample -llog -ljnigraphics -lGLESv1_CM -lGLESv2 -lz -ldl  -lgcc

LOCAL_CFLAGS += -DHAVE_AV_CONFIG_H -std=c99

LOCAL_MODULE := ffmpeg
LOCAL_MODULE_TAGS := libffmpeg
LOCAL_ARM_MODE := arm

include $(BUILD_SHARED_LIBRARY)
