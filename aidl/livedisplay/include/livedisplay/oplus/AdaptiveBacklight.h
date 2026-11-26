/*
 * SPDX-FileCopyrightText: 2025-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/lineage/livedisplay/BnAdaptiveBacklight.h>

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

class AdaptiveBacklight : public BnAdaptiveBacklight {
  public:
    AdaptiveBacklight() = default;

    // Methods from ::aidl::vendor::lineage::livedisplay::BnAdaptiveBacklight follow.
    ndk::ScopedAStatus getEnabled(bool* _aidl_return) override;
    ndk::ScopedAStatus setEnabled(bool enabled) override;
};

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
