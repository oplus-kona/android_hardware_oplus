/*
 * SPDX-FileCopyrightText: 2025-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AdaptiveBacklightService"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <livedisplay/oplus/AdaptiveBacklight.h>

using ::android::base::ReadFileToString;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

ndk::ScopedAStatus AdaptiveBacklight::getEnabled(bool* _aidl_return) {
    std::string value;
    if (!ReadFileToString(ADAPTIVE_BACKLIGHT_NODE, &value)) {
        LOG(ERROR) << "Failed to read current AdaptiveBacklight state from "
                   << ADAPTIVE_BACKLIGHT_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    const std::string trimmed = Trim(value);
    *_aidl_return = !trimmed.empty() && trimmed[0] != '0';
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus AdaptiveBacklight::setEnabled(bool enabled) {
    bool isEnabled = false;
    if (auto status = getEnabled(&isEnabled); !status.isOk()) {
        return status;
    }
    if (isEnabled != enabled &&
        !WriteStringToFile(enabled ? "1" : "0", ADAPTIVE_BACKLIGHT_NODE, true)) {
        LOG(ERROR) << "Failed to set AdaptiveBacklight state on " << ADAPTIVE_BACKLIGHT_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
