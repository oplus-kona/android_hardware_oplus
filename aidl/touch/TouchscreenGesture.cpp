/*
 * SPDX-FileCopyrightText: 2025-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.oplus"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <cstdlib>

#include <TouchscreenGestureConfig.h>

using ::android::base::ReadFileToString;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

namespace aidl {
namespace vendor {
namespace lineage {
namespace touch {

ndk::ScopedAStatus TouchscreenGesture::getSupportedGestures(std::vector<Gesture>* _aidl_return) {
    std::vector<Gesture> gestures;

    for (const auto& [id, name] : kGestureNames) {
        if (kSupportedGestures & (1 << id)) {
            gestures.push_back({static_cast<int>(gestures.size()), name, kGestureStartKey + id});
        }
    }

    *_aidl_return = gestures;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus TouchscreenGesture::setGestureEnabled(const Gesture& gesture, bool enabled) {
    const int offset = gesture.keycode - kGestureStartKey;
    if (offset < 0 || offset >= 31) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const int bit = 1 << offset;

    for (const auto& node : ::android::base::Split(GESTURE_ENABLE_INDEP_NODES, " ")) {
        if (node.empty()) continue;

        std::string tmp;
        if (!ReadFileToString(node, &tmp)) {
            LOG(ERROR) << "Failed to read gesture mask from " << node;
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }

        const std::string trimmed = Trim(tmp);
        char* end = nullptr;
        const long parsed = std::strtol(trimmed.c_str(), &end, 16);
        if (end == trimmed.c_str()) {
            LOG(ERROR) << "Invalid gesture mask '" << trimmed << "' in " << node;
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }

        const int contents = enabled ? (static_cast<int>(parsed) | bit)
                                     : (static_cast<int>(parsed) & ~bit);
        if (!WriteStringToFile(std::to_string(contents), node, true)) {
            LOG(ERROR) << "Failed to write gesture mask to " << node;
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace touch
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
