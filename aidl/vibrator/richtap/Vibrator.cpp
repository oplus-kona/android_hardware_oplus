/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"

#include <algorithm>
#include <chrono>
#include <cutils/properties.h>
#include <inttypes.h>
#include <log/log.h>

#ifdef USES_OPLUS_AWINIC
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <string>
#endif
#include <thread>

#include "aac_vibra_function.h"

#ifdef OVERRIDE_EFFECTS
#include "VibratorEffectsOverride.h"
#endif

#define RICHTAP_LIGHT_STRENGTH 69
#define RICHTAP_MEDIUM_STRENGTH 89
#define RICHTAP_STRONG_STRENGTH 99

#ifdef USES_OPLUS_AWINIC
#define RICHTAP_OPLUS_ACTIVATE_NODE "/sys/class/leds/vibrator/oplus_activate"
#define RICHTAP_OPLUS_DURATION_NODE "/sys/class/leds/vibrator/oplus_duration"
#define RICHTAP_OPLUS_STATE_NODE "/sys/class/leds/vibrator/oplus_state"
#endif

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

#ifdef USES_OPLUS_AWINIC
static int writeValue(const char* path, const char* value) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_WRONLY | O_CLOEXEC));
    if (fd < 0) {
        int savedErrno = errno;
        ALOGE("open %s failed, errno = %d", path, savedErrno);
        return -savedErrno;
    }

    size_t len = strlen(value);
    ssize_t ret = TEMP_FAILURE_RETRY(write(fd, value, len));
    int status;

    if (ret < 0) {
        status = -errno;
    } else if (static_cast<size_t>(ret) != len) {
        status = -EAGAIN;
    } else {
        status = 0;
    }

    close(fd);
    return status;
}

static int writeValue(const char* path, int value) {
    std::string strValue = std::to_string(value);
    return writeValue(path, strValue.c_str());
}

static int writeNode(const char* path, const char* value) {
    if (access(path, F_OK) != 0) {
        return -ENOENT;
    }

    return writeValue(path, value);
}

static int writeNode(const char* path, int value) {
    if (access(path, F_OK) != 0) {
        return -ENOENT;
    }

    return writeValue(path, value);
}

static bool hasSysfsOnOffSupport() {
    return access(RICHTAP_OPLUS_ACTIVATE_NODE, F_OK) == 0;
}

static int sysfsOn(int32_t timeoutMs) {
    int ret = writeNode(RICHTAP_OPLUS_STATE_NODE, "1");
    if (ret) {
        return ret;
    }

    ret = writeNode(RICHTAP_OPLUS_DURATION_NODE, timeoutMs);
    if (ret) {
        return ret;
    }

    return writeNode(RICHTAP_OPLUS_ACTIVATE_NODE, "1");
}

static int sysfsOff() {
    return writeNode(RICHTAP_OPLUS_ACTIVATE_NODE, "0");
}
#endif

Vibrator::Vibrator() {
    uint32_t deviceType = 0;

#ifdef USES_OPLUS_AWINIC
    mUseSysfsOnOff = hasSysfsOnOffSupport();
    if (mUseSysfsOnOff) {
        ALOGI("Using sysfs backend for on/off");
    }
#endif

    int32_t ret = aac_vibra_init(&deviceType);
    if (ret) {
        ALOGE("AAC init failed: %d\n", ret);
        return;
    }

    aac_vibra_looper_start();

    ALOGI("AAC init success: %u\n", deviceType);
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    mActiveCompositionId++;

#ifdef USES_OPLUS_AWINIC
    if (mUseSysfsOnOff) {
        int ret = sysfsOff();
        if (ret) {
            ALOGE("Sysfs off failed: %d\n", ret);
        }

        int32_t aacRet = aac_vibra_off();
        if (aacRet) {
            ALOGW("AAC off failed after sysfs off: %d\n", aacRet);
        }

        mCurrentAmplitude = 1.0f;
        return ndk::ScopedAStatus::ok();
    }
#endif

    int32_t ret = aac_vibra_off();
    if (ret) {
        ALOGE("AAC off failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    mCurrentAmplitude = 1.0f;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (timeoutMs <= 0) {
        return ndk::ScopedAStatus::ok();
    }

    mActiveCompositionId++;

    // Short tactile feedback pulses (e.g. keyboard taps, slider ticks, UI touches)
    // are translated to crisp RichTap tactile prebaked waveforms instead of continuous buzzes.
    if (timeoutMs <= 15) {
        int32_t strength = static_cast<int32_t>(mCurrentAmplitude * (55 - 35) + 35);
        strength = std::clamp(strength, 1, 99);
        int32_t ret = aac_vibra_looper_prebaked_effect(static_cast<uint32_t>(Effect::TICK), strength);
        int32_t playMs = (ret > 0) ? ret : timeoutMs;
        if (callback != nullptr) {
            std::thread([=] {
                usleep(playMs * 1000);
                callback->onComplete();
            }).detach();
        }
        return ndk::ScopedAStatus::ok();
    }

    if (timeoutMs <= 35) {
        int32_t strength = static_cast<int32_t>(mCurrentAmplitude * (RICHTAP_STRONG_STRENGTH - 50) + 50);
        strength = std::clamp(strength, 1, 99);
        int32_t ret = aac_vibra_looper_prebaked_effect(static_cast<uint32_t>(Effect::CLICK), strength);
        int32_t playMs = (ret > 0) ? ret : timeoutMs;
        if (callback != nullptr) {
            std::thread([=] {
                usleep(playMs * 1000);
                callback->onComplete();
            }).detach();
        }
        return ndk::ScopedAStatus::ok();
    }

#ifdef USES_OPLUS_AWINIC
    // For sustained long vibrations (>500ms, like incoming phone calls or alarms),
    // use the sysfs backend if available to ensure vibrations never cut off.
    if (mUseSysfsOnOff && timeoutMs > 500) {
        int ret = sysfsOn(timeoutMs);
        if (ret) {
            ALOGE("Sysfs on failed: %d\n", ret);
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
        }

        if (callback != nullptr) {
            std::thread([=] {
                usleep(timeoutMs * 1000);
                callback->onComplete();
            }).detach();
        }

        return ndk::ScopedAStatus::ok();
    }
#endif

    int32_t ret = aac_vibra_looper_on(timeoutMs);
    if (ret < 0) {
#ifdef USES_OPLUS_AWINIC
        if (mUseSysfsOnOff) {
            int sysRet = sysfsOn(timeoutMs);
            if (!sysRet) {
                if (callback != nullptr) {
                    std::thread([=] {
                        usleep(timeoutMs * 1000);
                        callback->onComplete();
                    }).detach();
                }
                return ndk::ScopedAStatus::ok();
            }
        }
#endif
        ALOGE("AAC on failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([=] {
            usleep(ret * 1000);
            callback->onComplete();
        }).detach();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength es,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    int32_t strength;
    uint32_t targetEffect;

#ifdef OVERRIDE_EFFECTS
    effect = overrideVibratorEffect(effect);
#endif

    mActiveCompositionId++;

    if (effect == Effect::TEXTURE_TICK) {
        targetEffect = static_cast<uint32_t>(Effect::TICK);
        switch (es) {
            case EffectStrength::LIGHT:
                strength = 40;
                break;
            case EffectStrength::MEDIUM:
                strength = 50;
                break;
            case EffectStrength::STRONG:
                strength = 60;
                break;
            default:
                return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    } else if (effect == Effect::TICK) {
        targetEffect = static_cast<uint32_t>(Effect::TICK);
        switch (es) {
            case EffectStrength::LIGHT:
                strength = 50;
                break;
            case EffectStrength::MEDIUM:
                strength = 65;
                break;
            case EffectStrength::STRONG:
                strength = 80;
                break;
            default:
                return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    } else if (effect == Effect::POP) {
        targetEffect = static_cast<uint32_t>(Effect::POP);
        switch (es) {
            case EffectStrength::LIGHT:
                strength = 60;
                break;
            case EffectStrength::MEDIUM:
                strength = 75;
                break;
            case EffectStrength::STRONG:
                strength = 90;
                break;
            default:
                return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    } else if (effect >= Effect::CLICK && effect <= Effect::HEAVY_CLICK) {
        targetEffect = static_cast<uint32_t>(effect);
        switch (es) {
            case EffectStrength::LIGHT:
                strength = RICHTAP_LIGHT_STRENGTH;
                break;
            case EffectStrength::MEDIUM:
                strength = RICHTAP_MEDIUM_STRENGTH;
                break;
            case EffectStrength::STRONG:
                strength = RICHTAP_STRONG_STRENGTH;
                break;
            default:
                return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    } else {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    int32_t ret = aac_vibra_looper_prebaked_effect(targetEffect, strength);
    if (ret < 0) {
        ALOGE("AAC perform failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([=] {
            usleep(ret * 1000);
            callback->onComplete();
        }).detach();
    }

    if (_aidl_return != nullptr) {
        *_aidl_return = ret;
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK,
                     Effect::THUD,  Effect::POP,          Effect::HEAVY_CLICK,
                     Effect::TEXTURE_TICK};

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    if (amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    mCurrentAmplitude = amplitude;
    uint8_t tmp = static_cast<uint8_t>(amplitude * 0xff);

    int32_t ret = aac_vibra_setAmplitude(tmp);
    if (ret) {
        ALOGW("AAC set amplitude failed: %d\n", ret);
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool enabled __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    if (maxDelayMs == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *maxDelayMs = 5000;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize) {
    if (maxSize == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *maxSize = 256;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(
        std::vector<CompositePrimitive>* supported) {
    if (supported == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *supported = {
            CompositePrimitive::NOOP,
            CompositePrimitive::CLICK,
            CompositePrimitive::THUD,
            CompositePrimitive::SPIN,
            CompositePrimitive::QUICK_RISE,
            CompositePrimitive::SLOW_RISE,
            CompositePrimitive::QUICK_FALL,
            CompositePrimitive::LIGHT_TICK,
            CompositePrimitive::LOW_TICK,
    };
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* durationMs) {
    if (durationMs == nullptr) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    switch (primitive) {
        case CompositePrimitive::NOOP:
            *durationMs = 0;
            break;
        case CompositePrimitive::CLICK:
            *durationMs = 15;
            break;
        case CompositePrimitive::THUD:
            *durationMs = 25;
            break;
        case CompositePrimitive::SPIN:
            *durationMs = 15;
            break;
        case CompositePrimitive::QUICK_RISE:
            *durationMs = 15;
            break;
        case CompositePrimitive::SLOW_RISE:
            *durationMs = 25;
            break;
        case CompositePrimitive::QUICK_FALL:
            *durationMs = 15;
            break;
        case CompositePrimitive::LIGHT_TICK:
            *durationMs = 8;
            break;
        case CompositePrimitive::LOW_TICK:
            *durationMs = 10;
            break;
        default:
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.size() > 256) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    std::vector<CompositePrimitive> supported;
    getSupportedPrimitives(&supported);

    for (const auto& e : composite) {
        if (e.delayMs < 0 || e.delayMs > 5000) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
        }
        if (e.scale < 0.0f || e.scale > 1.0f) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
        }
        if (std::find(supported.begin(), supported.end(), e.primitive) == supported.end()) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    }

    uint64_t myId = ++mActiveCompositionId;

    std::thread([this, composite, callback, myId]() {
        for (const auto& e : composite) {
            if (mActiveCompositionId != myId) {
                return;
            }

            if (e.delayMs > 0) {
                usleep(e.delayMs * 1000);
                if (mActiveCompositionId != myId) {
                    return;
                }
            }

            if (e.primitive == CompositePrimitive::NOOP) {
                continue;
            }

            uint32_t effectId;
            int32_t strength;

            switch (e.primitive) {
                case CompositePrimitive::CLICK:
                    effectId = static_cast<uint32_t>(Effect::CLICK);
                    strength = static_cast<int32_t>(e.scale * (RICHTAP_STRONG_STRENGTH - 50) + 50);
                    break;
                case CompositePrimitive::THUD:
                    effectId = static_cast<uint32_t>(Effect::THUD);
                    strength = static_cast<int32_t>(e.scale * (RICHTAP_STRONG_STRENGTH - 50) + 50);
                    break;
                case CompositePrimitive::SPIN:
                    effectId = static_cast<uint32_t>(Effect::POP);
                    strength = static_cast<int32_t>(e.scale * (RICHTAP_STRONG_STRENGTH - 45) + 45);
                    break;
                case CompositePrimitive::QUICK_RISE:
                    effectId = static_cast<uint32_t>(Effect::POP);
                    strength = static_cast<int32_t>(e.scale * (RICHTAP_STRONG_STRENGTH - 45) + 45);
                    break;
                case CompositePrimitive::SLOW_RISE:
                    effectId = static_cast<uint32_t>(Effect::THUD);
                    strength = static_cast<int32_t>(e.scale * (RICHTAP_STRONG_STRENGTH - 45) + 45);
                    break;
                case CompositePrimitive::QUICK_FALL:
                    effectId = static_cast<uint32_t>(Effect::POP);
                    strength = static_cast<int32_t>(e.scale * (RICHTAP_STRONG_STRENGTH - 45) + 45);
                    break;
                case CompositePrimitive::LIGHT_TICK:
                    effectId = static_cast<uint32_t>(Effect::TICK);
                    strength = static_cast<int32_t>(e.scale * (55 - 35) + 35);
                    break;
                case CompositePrimitive::LOW_TICK:
                    effectId = static_cast<uint32_t>(Effect::TICK);
                    strength = static_cast<int32_t>(e.scale * (65 - 40) + 40);
                    break;
                default:
                    continue;
            }

            strength = std::clamp(strength, 1, 99);
            int32_t dur = aac_vibra_looper_prebaked_effect(effectId, strength);
            int32_t waitMs = (dur > 0) ? dur : 10;
            usleep(waitMs * 1000);
        }

        if (mActiveCompositionId == myId && callback != nullptr) {
            callback->onComplete();
        }
    }).detach();

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(
        std::vector<Effect>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t id __unused, Effect effect __unused,
                                            EffectStrength strength __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t id __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* freqResolutionHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* freqMinimumHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* supported __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& composite __unused,
                                         const std::shared_ptr<IVibratorCallback>& callback
                                                 __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
