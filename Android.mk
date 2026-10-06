LOCAL_PATH := $(call my-dir)

# The makefiles below define module names that device/htc/msm8974-common
# defines for its own platform (libloc_core, libgps.utils, libloc_eng,
# gnss_xtra_probe, libwcnss_qmi). Kati rejects the second definition of a
# module name, so both trees load their makefiles only for their own
# TARGET_BOARD_PLATFORM.
ifeq ($(BOARD_VENDOR),htc)
ifeq ($(TARGET_BOARD_PLATFORM),msm8226)
include $(call all-subdir-makefiles,$(LOCAL_PATH))
endif
endif
