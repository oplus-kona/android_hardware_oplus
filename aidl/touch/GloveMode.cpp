/*
 * SPDX-FileCopyrightText: 2025-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.oplus"

#include "GloveMode.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

using ::android::base::ReadFileToString;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

namespace aidl {
namespace vendor {
namespace lineage {
namespace touch {

ndk::ScopedAStatus GloveMode::getEnabled(bool* _aidl_return) {
    std::string value;

    if (!ReadFileToString(GLOVE_MODE_NODE, &value)) {
        LOG(ERROR) << "Failed to read current GloveMode state from " << GLOVE_MODE_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    const std::string trimmed = Trim(value);
    *_aidl_return = !trimmed.empty() && trimmed[0] != '0';
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus GloveMode::setEnabled(bool enable) {
    if (!WriteStringToFile(enable ? "1" : "0", GLOVE_MODE_NODE, true)) {
        LOG(ERROR) << "Failed to write GloveMode state to " << GLOVE_MODE_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace touch
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
