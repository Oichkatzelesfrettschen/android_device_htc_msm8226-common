#!/bin/bash
#
# Copyright (C) 2017-2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

set -e

if [ -z "${BOARD_COMMON}" ]; then
    echo ""
    echo "error: This is a script in a common tree. Please execute" $(basename $0) "from a device tree."
    echo ""
    exit 1
fi

# Override anything that may come from the calling environment
DEVICE_COMMON="${BOARD_COMMON}"

# Load extract_utils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "${MY_DIR}" ]]; then MY_DIR="${PWD}"; fi

ANDROID_ROOT="${MY_DIR}/../../.."

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"

setup_vendor "${BOARD_COMMON}" "${VENDOR}" "${ANDROID_ROOT}" true

# The a11chl blobs live at vendor/htc-a11chl, outside the vendor/htc project
# other HTC device trees share. setup_vendor derives vendor/$VENDOR/$DEVICE,
# so the output directory and the four files generated in it point there.
export OUTDIR=vendor/htc-a11chl
export PRODUCTMK="${ANDROID_ROOT}/${OUTDIR}/${BOARD_COMMON}-vendor.mk"
export ANDROIDBP="${ANDROID_ROOT}/${OUTDIR}/Android.bp"
export ANDROIDMK="${ANDROID_ROOT}/${OUTDIR}/Android.mk"
export BOARDMK="${ANDROID_ROOT}/${OUTDIR}/BoardConfigVendor.mk"

extract "${MY_DIR}/common-proprietary-files.txt" "${SRC}"
