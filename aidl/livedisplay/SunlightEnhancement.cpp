/*
 * SPDX-FileCopyrightText: 2022-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "SunlightEnhancementService"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <livedisplay/oplus/SunlightEnhancement.h>

using ::android::base::ReadFileToString;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

ndk::ScopedAStatus SunlightEnhancement::getEnabled(bool* _aidl_return) {
    std::string value;
    if (!ReadFileToString(HBM_NODE, &value)) {
        LOG(ERROR) << "Failed to read current SunlightEnhancement state from " << HBM_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    const std::string trimmed = Trim(value);
    *_aidl_return = !trimmed.empty() && trimmed[0] != '0';
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SunlightEnhancement::setEnabled(bool enabled) {
    bool isEnabled = false;
    if (auto status = getEnabled(&isEnabled); !status.isOk()) {
        return status;
    }
    if (isEnabled != enabled && !WriteStringToFile(enabled ? "1" : "0", HBM_NODE, true)) {
        LOG(ERROR) << "Failed to set SunlightEnhancement state on " << HBM_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
