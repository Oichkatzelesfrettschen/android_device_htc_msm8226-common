#
# Copyright (C) 2018 The LineageOS Project
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
#

# HTC services add policy under this directory as their interfaces qualify.
BOARD_VENDOR_SEPOLICY_DIRS += device/htc/msm8226-common/sepolicy/common

# storaged is a platform-private domain, so its rule lives in system_ext
# private policy and the type it reads in system_ext public policy.
SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/htc/msm8226-common/sepolicy/public
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/htc/msm8226-common/sepolicy/private

# Domains and labels for the vendor HAL services that this tree and
# device/htc/a11 build.
BOARD_VENDOR_SEPOLICY_DIRS += device/htc/msm8226-common/sepolicy/hal
