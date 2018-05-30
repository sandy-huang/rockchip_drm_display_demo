LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
include $(LOCAL_PATH)/Makefile.sources
include $(LIBDRM_COMMON_MK)

LOCAL_SRC_FILES := $(filter-out %.h,$(MODETEST_FILES))

LOCAL_MODULE := drm_display_demo

LOCAL_FORCE_STATIC_EXECUTABLE := true

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../../ \
	$(LOCAL_PATH)/../../include/ \
        $(LOCAL_PATH)/../../include/drm

#LOCAL_SHARED_LIBRARIES := libdrm
LOCAL_STATIC_LIBRARIES := libdrm_util \
			  libc \
			  libm \
			  libdrm \
			  libdrm_rockchip

#LOCAL_SHARED_LIBRARIES += libdrm_rockchip

include $(BUILD_EXECUTABLE)
