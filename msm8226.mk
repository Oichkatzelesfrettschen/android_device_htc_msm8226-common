# Copyright (C) 2012 The CyanogenMod Project
# Copyright (C) 2017-2021 The LineageOS Project
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


# Permissions
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/etc/init.qcom.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.qcom.rc \
    $(LOCAL_PATH)/rootdir/etc/rild.sprint.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/rild.sprint.rc \
    $(LOCAL_PATH)/rootdir/etc/init.cache-recovery.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/init.cache-recovery.rc \
    $(LOCAL_PATH)/rootdir/etc/ueventd.qcom.rc:$(TARGET_COPY_OUT_VENDOR)/ueventd.rc \
    $(LOCAL_PATH)/rootdir/bin/init.qcom.bt.sh:$(TARGET_COPY_OUT_VENDOR)/bin/init.qcom.bt.sh \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.camera.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.xml \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.direct.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.software.sip.voip.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.sip.voip.xml \
    frameworks/native/data/etc/android.software.midi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.midi.xml

# APEX: compressed APEX packages depend on dm-verity, so the product installs
# its APEXes uncompressed.
PRODUCT_COMPRESSED_APEX := false
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/ld.config.txt:$(TARGET_COPY_OUT_SYSTEM)/etc/swcodec/ld.config.txt

# Audio
PRODUCT_PACKAGES += \
    android.hardware.audio@6.0-impl \
    android.hardware.audio@2.0-service \
    android.hardware.audio.effect@6.0-impl \
    audio.primary.msm8226 \
    audio.r_submix.default \
    audio.usb.default \
    libaudio-resampler \
    libqcomvisualizer \
    libqcompostprocbundle \
    libqcomvoiceprocessing

# Bluetooth
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-impl \
    android.hardware.bluetooth@1.0-service \
    libbt-vendor

# Boot animation
TARGET_BOOTANIMATION_HALF_RES := true

# Display
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.graphics.mapper@2.0-impl-2.1 \
    android.hardware.memtrack@1.0-impl \
    android.hardware.memtrack@1.0-service \
    copybit.msm8226 \
    gralloc.msm8226 \
    hwcomposer.msm8226 \
    memtrack.msm8226

# Camera
PRODUCT_PACKAGES += \
    android.hardware.camera.provider@2.4-impl \
    android.hardware.camera.provider@2.4-service \
    camera.device@1.0-impl \
    libxml2 \
    Aperture

# DRM: android.hardware.drm@1.0-service loads each plugin in
# /vendor/lib/mediadrm. vendor/htc-a11chl installs the Widevine L3 plugin,
# whose NEEDED protobuf library is libprotobuf-cpp-lite-v29.so, and the plugin
# keeps its state in /data/vendor/w (rootdir/etc/init.drm.rc).
PRODUCT_PACKAGES += \
    android.hardware.drm@1.0-impl \
    android.hardware.drm@1.0-service \
    libprotobuf-cpp-lite-v29

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/etc/init.drm.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.drm.rc

# fastbootd
PRODUCT_PACKAGES += \
    fastbootd

# FM Radio: the app opens the Iris tuner through libfmjni's Qualcomm backend.
PRODUCT_PACKAGES += \
    FMRadio \
    libfmjni

# Gatekeeper
PRODUCT_PACKAGES += \
    android.hardware.gatekeeper@1.0-service.software

# GPS
PRODUCT_PACKAGES += \
    android.hardware.gnss@1.0-impl.legacy \
    android.hardware.gnss@1.0-service.legacy \
    gps.msm8226

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/gps/etc/flp.conf:$(TARGET_COPY_OUT_VENDOR)/etc/flp.conf \
    $(LOCAL_PATH)/gps/etc/gps.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gps.conf \
    $(LOCAL_PATH)/gps/etc/izat.conf:$(TARGET_COPY_OUT_VENDOR)/etc/izat.conf \
    $(LOCAL_PATH)/gps/etc/sap.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sap.conf

PRODUCT_PACKAGES += \
    msm8226_gnss_config_link_flp_conf \
    msm8226_gnss_config_link_gps_conf \
    msm8226_gnss_config_link_izat_conf \
    msm8226_gnss_config_link_sap_conf

# Health
PRODUCT_PACKAGES += \
    android.hardware.health@2.1-impl \
    android.hardware.health@2.1-service

# Keylayouts
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/keylayout/Button_Jack.kl:system/usr/keylayout/Button_Jack.kl \
    $(LOCAL_PATH)/keylayout/synaptics_rmi4_i2c.kl:system/usr/keylayout/synaptics_rmi4_i2c.kl

# Keymaster
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-impl \
    android.hardware.keymaster@3.0-service

# Lights: the amber and green LEDs and the panel backlight, driven from sysfs.
PRODUCT_PACKAGES += \
    android.hardware.light@2.0-service.htc_msm8226

# LiveDisplay
PRODUCT_PACKAGES += \

# Media
PRODUCT_COPY_FILES += \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_video_le.xml \
    device/htc/msm8226-common/configs/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    device/htc/msm8226-common/configs/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml

# ASF demuxer for the WMA, VC-1 and MPEG-4 streams in .wma, .wmv and .asf files.
PRODUCT_PACKAGES += libasfextractor

# AVI demuxer for the DivX, MPEG-4, MP3, AAC and PCM streams in .avi files, and
# FLV demuxer for the Sorenson Spark, MP3 and AAC streams in .flv files.
PRODUCT_PACKAGES += libaviextractor libflvextractor

# Network
PRODUCT_PACKAGES += liba11-legacy-radio

# Tethering requires the separate NetworkStack APK's signature permission.
PRODUCT_PACKAGES += \
    NetworkStack

# Offline Charger
PRODUCT_PACKAGES += \
    charger_res_images

# OMX
PRODUCT_PACKAGES += \
    libc2dcolorconvert \
    libOmxAacEnc \
    libOmxAmrEnc \
    libOmxCore \
    libOmxEvrcEnc \
    libOmxQcelp13Enc \
    libOmxVdec \
    libOmxVenc \
    libstagefrighthw

# OpenCL: apps reach the Adreno ICD in /vendor/lib through the vendor public
# library list, which adds libOpenCL.so to every app's linker namespace.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/public.libraries.txt:$(TARGET_COPY_OUT_VENDOR)/etc/public.libraries.txt

# Sensors: the multihal loads the sub-HALs listed in
# /vendor/etc/sensors/_hals.conf; with no list it serves ISensors with no
# sensors, which lets SensorService start.
PRODUCT_PACKAGES += \
    android.hardware.sensors@1.0-impl.htc8226 \
    android.hardware.sensors@1.0-service.htc8226 \
    sensors.msm8226

# Power HAL
PRODUCT_PACKAGES += \
    android.hardware.power-service-qti

# Speed-preopt interactive apps to avoid startup JIT work on Cortex-A7.
# common_mobile.mk speed-preopts the selected Go launcher; omit the unused
# TrebuchetQuickStep entry so the list matches the installed launcher.
PRODUCT_DEXPREOPT_SPEED_APPS += \
    Settings \
    Aperture \
    SystemUI

# Radio: the wrapper serves IRadio 1.4 to the framework and calls the IRadio
# 1.0 HAL that rild registers for the same slot.
PRODUCT_PACKAGES += \
    android.hardware.radio@1.4-service.legacy

# RenderScript HAL
PRODUCT_PACKAGES += \
    android.hardware.renderscript@1.0-impl

# Seccomp
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/seccomp/mediacodec.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/mediacodec.policy

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# TimeKeep
PRODUCT_PACKAGES += \
    timekeep \
    TimeKeep

# USB
PRODUCT_PACKAGES += \
    android.hardware.usb@1.3-service.basic

# Vibrator: device/htc/a11 builds android.hardware.vibrator-service.a11, which
# serves IVibrator with amplitude control over the timed_output node.

# Wifi
PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0-service.legacy \
    hostapd \
    wpa_supplicant \
    wpa_supplicant.conf \
    libwpa_client

# hardware/qcom-caf/wlan/wcnss-service installs to /vendor only when this is
# true; vendor policy labels only vendor executables.
PRODUCT_VENDOR_MOVE_ENABLED := true
PRODUCT_PACKAGES += \
    libcurl \
    wcnss_service

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf \
    $(LOCAL_PATH)/configs/wifi/p2p_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant_overlay.conf \
    $(LOCAL_PATH)/configs/wifi/hostapd_default.conf:$(TARGET_COPY_OUT_SYSTEM)/etc/hostapd/hostapd_default.conf

# Get non-open-source specific aspects
$(call inherit-product, vendor/htc-a11chl/a11chl-vendor.mk)

# common msm8226
$(call inherit-product, device/samsung/qcom-common/qcom-common.mk)
