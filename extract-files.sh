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

# The Widevine L3 plugin builds its per-app certificate, usage table and L3
# state path from the literal "/data/mediadrm/IDM", and the drm HAL runs as a
# vendor domain that reaches /data/vendor only. The fixup swaps the 18-byte
# literal for "/data/vendor/w/IDM", which leaves the ELF layout and every other
# byte unchanged. It accepts the donor build (sha256 3424eb26...) and rewrites it
# to sha256 7f4a9703..., accepts that result unchanged, and stops the extraction
# on any other input. vendor/htc-a11chl/tools/prepare-widevine-l3.sh applies the
# same transform.
function widevine_l3_storage_fixup() {
    local file="${1}"
    local donor_sha256=3424eb2605ed52082106d1f03b425da9e9b482381f65f16c3e4bd65937e245ef
    local patched_sha256=7f4a97033a1204ddfe685cc0d3d9b63dbb87b1a89f6c64521814316e951f99a2
    local sum tmp

    sum=$(sha256sum "${file}" | awk '{print $1}')
    if [ "${sum}" = "${patched_sha256}" ]; then
        return 0
    fi
    if [ "${sum}" != "${donor_sha256}" ]; then
        echo "error: ${file} has sha256 ${sum}; the Widevine L3 fixup accepts ${donor_sha256} or ${patched_sha256}" >&2
        exit 1
    fi

    tmp=$(mktemp "${file}.XXXXXX")
    perl -0777 -e '
        binmode(STDIN) or die "binmode: $!";
        binmode(STDOUT) or die "binmode: $!";
        my $data = do { local $/; <STDIN> };
        my $n = ($data =~ s{/data/mediadrm/IDM}{/data/vendor/w/IDM}g);
        $n == 1 or die "expected one /data/mediadrm/IDM literal, found $n\n";
        index($data, "/data/mediadrm") < 0 or die "a /data/mediadrm literal remains\n";
        print {*STDOUT} $data or die "write: $!";
    ' < "${file}" > "${tmp}" || { rm -f -- "${tmp}"; exit 1; }
    sum=$(sha256sum "${tmp}" | awk '{print $1}')
    if [ "${sum}" != "${patched_sha256}" ]; then
        echo "error: the Widevine L3 fixup produced sha256 ${sum}, expected ${patched_sha256}" >&2
        rm -f -- "${tmp}"
        exit 1
    fi
    chmod --reference="${file}" -- "${tmp}"
    mv -f -- "${tmp}" "${file}"
}

# The Sprint QCRIL opens its database at /data/misc/radio/qcril.db, a core
# data path that vendor rild may not write under Treble. The fixup points it
# at /dev/radio/qcril.db, the tmpfs directory init.qcom.rc creates, with the
# six freed bytes zeroed so the string keeps its offset and length.
function blob_fixup() {
    case "${1}" in
        vendor/lib/libril-qc-qmi-1.so)
            [ "$2" = "" ] && return 0
            sed -i 's|/data/misc/radio/qcril.db|/dev/radio/qcril.db\x00\x00\x00\x00\x00\x00|' "${2}"
            ;;
        vendor/lib/mediadrm/libwvdrmengine.so)
            [ "$2" = "" ] && return 0
            widevine_l3_storage_fixup "${2}"
            ;;
        *)
            return 1
            ;;
    esac
    return 0
}

extract "${MY_DIR}/common-proprietary-files.txt" "${SRC}"
