/*
 * SPDX-FileCopyrightText: 2019-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "DisplayModesService"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <livedisplay/oplus/DisplayModes.h>
#include <fstream>

using ::android::base::WriteStringToFile;

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

static const std::string kDefaultPath = "/data/vendor/display/default_display_mode";

const std::map<int32_t, DisplayModes::ModeInfo> DisplayModes::kModeMap = {
        {0, {"Vivid", 0}},
        {1, {"Natural", 1}},
        {2, {"Cinematic", 2}},
        {3, {"Brilliant", 3}},
};

DisplayModes::DisplayModes() : mCurrentModeId(0), mDefaultModeId(0) {
    std::ifstream defaultFile(kDefaultPath);
    int32_t savedMode = 0;
    if (defaultFile >> savedMode && kModeMap.find(savedMode) != kModeMap.end()) {
        mDefaultModeId = savedMode;
    }
    setDisplayMode(mDefaultModeId, false);
}

ndk::ScopedAStatus DisplayModes::getDisplayModes(std::vector<DisplayMode>* _aidl_return) {
    std::vector<DisplayMode> modes;

    for (const auto& entry : kModeMap) {
        modes.push_back({entry.first, entry.second.name});
    }
    *_aidl_return = modes;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DisplayModes::getCurrentDisplayMode(DisplayMode* _aidl_return) {
    *_aidl_return = {mCurrentModeId, kModeMap.at(mCurrentModeId).name};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DisplayModes::getDefaultDisplayMode(DisplayMode* _aidl_return) {
    *_aidl_return = {mDefaultModeId, kModeMap.at(mDefaultModeId).name};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DisplayModes::setDisplayMode(int32_t modeID, bool makeDefault) {
    const auto iter = kModeMap.find(modeID);
    if (iter == kModeMap.end()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (!WriteStringToFile(std::to_string(iter->second.seedValue), DISPLAY_MODES_NODE, true)) {
        LOG(ERROR) << "Failed to write DisplayMode " << modeID << " to " << DISPLAY_MODES_NODE;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    mCurrentModeId = iter->first;
    if (makeDefault) {
        std::ofstream defaultFile(kDefaultPath);
        defaultFile << iter->first;
        if (!defaultFile.fail()) {
            mDefaultModeId = iter->first;
        }
    }
    if (mOnDisplayModeSet) {
        mOnDisplayModeSet();
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
