/*
 * SPDX-FileCopyrightText: 2024-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aidl/android/hardware/power/BnPower.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <cstdlib>

using aidl::android::hardware::power::Mode;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::Trim;
using android::base::WriteStringToFile;

namespace {
constexpr int kDoubleTapGestureBit = 1 << 1;
}  // anonymous namespace

#ifdef LIBPERFMGR_EXT
namespace aidl::google::hardware::power::impl::pixel {
#else
namespace aidl::android::hardware::power::impl {
#endif

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return) {
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE:
            *_aidl_return = true;
            return true;
        default:
            return false;
    }
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE: {
            LOG(INFO) << "Power mode: " << toString(type) << " isDoubleTapEnabled: " << enabled;
            bool ok = true;

            for (const auto& node : Split(GESTURE_ENABLE_INDEP_NODES, " ")) {
                if (node.empty()) continue;

                std::string tmp;
                if (!ReadFileToString(node, &tmp)) {
                    LOG(ERROR) << "Failed to read gesture mask from " << node;
                    ok = false;
                    continue;
                }

                const std::string trimmed = Trim(tmp);
                char* end = nullptr;
                const long parsed = std::strtol(trimmed.c_str(), &end, 16);
                if (end == trimmed.c_str()) {
                    LOG(ERROR) << "Invalid gesture mask '" << trimmed << "' in " << node;
                    ok = false;
                    continue;
                }

                int contents = static_cast<int>(parsed);
                if (enabled) {
                    contents |= kDoubleTapGestureBit;
                } else {
                    contents &= ~kDoubleTapGestureBit;
                }

                if (!WriteStringToFile(std::to_string(contents), node, true)) {
                    LOG(ERROR) << "Failed to write gesture mask to " << node;
                    ok = false;
                }
            }
            return ok;
        }
        default:
            return false;
    }
}

}  // namespace
