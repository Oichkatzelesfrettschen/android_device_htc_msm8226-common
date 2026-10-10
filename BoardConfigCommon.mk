# Copyright (C) 2012 The CyanogenMod Project
# Copyright (C) 2017-2018,2021 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# inherit from qcom-common
include device/samsung/qcom-common/BoardConfigCommon.mk

# qcom-common sets BOARD_VENDOR to samsung. Android.mk in this tree and in
# device/htc/msm8974-common gate on BOARD_VENDOR being htc.
BOARD_VENDOR := htc

# qcom-common quotes the pixel format, and soong_config.mk copies the quotes
# into the JSON string of soong.<product>.extra.variables, which
# product_config then fails to parse.
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888

# Non-A/B: build/make/core/board_config.mk defaults AB_OTA_UPDATER to true
# for a device that leaves it unset, and the updater-script flow needs false.
AB_OTA_UPDATER := false

# Platform
TARGET_BOARD_PLATFORM := msm8226
TARGET_BOARD_PLATFORM_GPU := qcom-adreno305

# Architecture
TARGET_CPU_VARIANT := generic
TARGET_CPU_VARIANT_RUNTIME := krait

# Audio
AUDIO_FEATURE_ENABLED_COMPRESS_VOIP := true
AUDIO_FEATURE_ENABLED_EXTN_FORMATS := true
AUDIO_FEATURE_ENABLED_EXTN_POST_PROC := true
# FLAC_OFFLOAD_ENABLED reads snd_dec_flac from the kernel's compress_params.h, so
# kernel/htc/a11 must carry the compress-offload UAPI before this flag builds.
AUDIO_FEATURE_ENABLED_FLAC_OFFLOAD := true
AUDIO_FEATURE_ENABLED_FLUENCE := true
AUDIO_FEATURE_ENABLED_HFP := true
AUDIO_FEATURE_ENABLED_HIFI_AUDIO := true
AUDIO_FEATURE_ENABLED_PROXY_DEVICE := true
AUDIO_FEATURE_ENABLED_SPEECH_OFFLOAD := true
# WMA_OFFLOAD_ENABLED reads snd_enc_wma.avg_bit_rate from the kernel's compress_params.h.
AUDIO_FEATURE_ENABLED_WMA_OFFLOAD := true
AUDIO_FEATURE_ENABLED_LOW_LATENCY_CAPTURE := true
BOARD_USES_ALSA_AUDIO := true

# Bionic
MALLOC_SVELTE := true

# Bluetooth
BOARD_HAVE_BLUETOOTH := true

# Camera
TARGET_USES_MEDIA_EXTENSIONS := true
# The HTC camera module is HALv1 only, and the 2.4 provider publishes its
# cameras as HIDL device@1.0; frameworks/av serves those devices to Camera1
# clients through CameraClient when this is set.
$(call soong_config_set,camera,legacy_hal1,true)

# Media
# frameworks/av lists the Qualcomm WMA, AMR-WB+ and VC-1 OMX decoders in
# GetComponentRole and configures them in ACodec when this is set.
$(call soong_config_set,stagefright,omx_legacy_qcom_codecs,true)
# Adds libcodec2_soft_a11vp6dec (media/vp6) to the software Codec2 runtime libraries.
$(call soong_config_set,stagefright,a11_vp6_codec2,true)

# Dexpreopt
ifeq ($(HOST_OS),linux)
  ifneq ($(TARGET_BUILD_VARIANT),eng)
    WITH_DEXPREOPT_BOOT_IMG_AND_SYSTEM_SERVER_ONLY ?= false
    WITH_DEXPREOPT := true
  endif
endif

# Display
TARGET_ADDITIONAL_GRALLOC_10_USAGE_BITS := 0x2000U | 0x02000000U
TARGET_DISABLE_POSTRENDER_CLEANUP := true

# Shader cache config options
# Maximum size of the  GLES Shaders that can be cached for reuse.
# Increase the size if shaders of size greater than 12KB are used.
MAX_EGL_CACHE_KEY_SIZE := 12*1024

# Maximum GLES shader cache size for each app to store the compiled shader
# binaries. Decrease the size if RAM or Flash Storage size is a limitation
# of the device.
MAX_EGL_CACHE_SIZE := 2048*1024

# FM Radio
# The Iris SMD transport is built into the kernel and registers on the first
# open of /dev/radio0, so libfmjni downloads no firmware and starts no loader
# service; its only handshake is the hw.fm.init readiness property.
$(call soong_config_set,libfmjni,vendor,qcom)
$(call soong_config_set_bool,libfmjni,no_fm_firmware,true)

# Filesystem
TARGET_FS_CONFIG_GEN := device/htc/msm8226-common/config.fs

# HIDL
DEVICE_MANIFEST_FILE := device/htc/msm8226-common/manifest.xml
DEVICE_MATRIX_FILE := device/htc/msm8226-common/compatibility_matrix.xml
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE += device/htc/msm8226-common/framework_compatibility_matrix.xml
PRODUCT_ENFORCE_VINTF_MANIFEST_OVERRIDE := true

# Build
# The HTC vendor blobs install through PRODUCT_COPY_FILES from
# vendor/htc-a11chl; Android 12's check-elf-prebuilt-product-copy-files
# rejects ELF files there unless this is set.
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# Kernel
# The ramdisks are xz (plain LZMA2), which the kernel unpacks with
# CONFIG_RD_XZ: a gzip recovery ramdisk puts recovery.img above its 16 MiB
# partition. build/make/core/Makefile runs $(XZ) for BOARD_RAMDISK_USE_XZ and
# nothing in the tree defines it.
BOARD_RAMDISK_USE_XZ := true
XZ := prebuilts/build-tools/$(HOST_PREBUILT_TAG)/bin/xz
# The arm-linux-androideabi GCC 4.9 prebuilt builds the kernel by default.
# A11_KERNEL_CLANG_THINLTO=true builds it with Android Clang 22
# (clang-r584948) under LTO_CLANG_THIN: kernel.mk puts the clang directory
# first in PATH, passes CC="ccache clang" and sets LTO_CLANG_THIN from
# KERNEL_LTO; LLVM=1 selects ld.lld and the LLVM binutils, and the kernel's
# CLANG_FLAGS carry the ARM EABI target past the command-line CC. LD rather
# than LDFLAGS carries --fatal-warnings there, because a command-line LDFLAGS
# replaces the ARM linker emulation arch/arm/Makefile adds for ld.lld. The
# compiler is AOSP platform/prebuilts/clang/host/linux-x86 at tag
# android-17.0.0_r1 (commit 29182889), directory clang-r584948, which the
# LineageOS manifest does not sync, so its absence stops the build rather
# than letting PATH fall through to the host's clang.
ifeq ($(A11_KERNEL_CLANG_THINLTO),true)
TARGET_KERNEL_CLANG_COMPILE := true
TARGET_KERNEL_CLANG_VERSION := r584948
ifeq ($(wildcard prebuilts/clang/host/$(HOST_PREBUILT_TAG)/clang-$(TARGET_KERNEL_CLANG_VERSION)/bin/clang),)
$(error prebuilts/clang/host/$(HOST_PREBUILT_TAG)/clang-$(TARGET_KERNEL_CLANG_VERSION) is absent: provision clang-$(TARGET_KERNEL_CLANG_VERSION) from AOSP platform/prebuilts/clang/host/linux-x86 android-17.0.0_r1 (29182889) there)
endif
KERNEL_LTO := thin
TARGET_KERNEL_ADDITIONAL_FLAGS := \
    -j2 HOSTCFLAGS="-fcommon -Werror" KCFLAGS=-Werror \
    LLVM=1 LD="ld.lld --fatal-warnings"
else
TARGET_KERNEL_CLANG_COMPILE := false
TARGET_KERNEL_ADDITIONAL_FLAGS := \
    -j2 HOSTCFLAGS="-fcommon -Werror" HOSTLDFLAGS="-fuse-ld=lld" KCFLAGS=-Werror \
    LDFLAGS=--fatal-warnings
endif

# Legacy memfd
TARGET_HAS_MEMFD_BACKPORT := true

# SELinux
include device/htc/msm8226-common/sepolicy/sepolicy.mk

# Partitions
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_USE_F2FS := true
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VOLD_EMMC_SHARES_DEV_MAJOR := true
# The read-only root carries the vfat firmware mountpoints that fstab.qcom uses.
BOARD_ROOT_EXTRA_FOLDERS := efs firmware firmware/adsp firmware/radio firmware/wcnss firmware-modem persist
BOARD_ROOT_EXTRA_SYMLINKS := \
    /data/tombstones:/tombstones

# Netd
TARGET_NEEDS_NETD_DIRECT_CONNECT_RULE := true

# Power
TARGET_USES_INTERACTION_BOOST := true

# Properties
TARGET_SYSTEM_PROP += device/htc/msm8226-common/system.prop

# Recovery
TARGET_RECOVERY_DEVICE_DIRS += device/htc/msm8226-common

# Time services
BOARD_USES_QC_TIME_SERVICES := true

# VNDK - Dedupe VNDK libraries with identical core variants.
TARGET_VNDK_USE_CORE_VARIANT := true

# Wifi
BOARD_WLAN_DEVICE                := qcwcn
BOARD_HAS_QCOM_WLAN              := true
BOARD_HAS_QCOM_WLAN_SDK          := true
BOARD_HOSTAPD_DRIVER             := NL80211
BOARD_HOSTAPD_PRIVATE_LIB        := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_WPA_SUPPLICANT_DRIVER      := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
TARGET_USES_QCOM_WCNSS_QMI       := false
TARGET_USES_WCNSS_CTRL           := true
WPA_SUPPLICANT_VERSION           := VER_0_8_X
# The Pronto WLAN driver is built into the kernel and initializes when the HAL
# writes WIFI_DRIVER_FW_PATH_STA to /sys/module/wlan/parameters/fwpath; the
# HAL loads no module.
WIFI_DRIVER_FW_PATH_STA          := "sta"
WIFI_DRIVER_FW_PATH_AP           := "ap"
WIFI_HIDL_UNIFIED_SUPPLICANT_SERVICE_RC_ENTRY := true

# inherit from the proprietary version
include vendor/htc-a11chl/BoardConfigVendor.mk
