/*
 * SPDX-FileCopyrightText: 2022-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AntiFlickerService"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <livedisplay/oplus/AntiFlicker.h>

using ::android::base::ReadFileToString;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

ndk::ScopedAStatus AntiFlicker::getEnabled(bool* _aidl_return) {
    std::string value;
    if (!ReadFileToString(ANTI_FLICKER_NODE, &value)) {
        LOG(ERROR) << "Failed to read current AntiFlicker state from " << ANTI_FLICKER_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    const std::string trimmed = Trim(value);
    *_aidl_return = !trimmed.empty() && trimmed[0] != '0';
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus AntiFlicker::setEnabled(bool enabled) {
    bool isEnabled = false;
    if (auto status = getEnabled(&isEnabled); !status.isOk()) {
        return status;
    }
    if (isEnabled != enabled &&
        !WriteStringToFile(enabled ? "1" : "0", ANTI_FLICKER_NODE, true)) {
        LOG(ERROR) << "Failed to set AntiFlicker state on " << ANTI_FLICKER_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
