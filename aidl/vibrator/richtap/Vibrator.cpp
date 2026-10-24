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

// RichTap Prebaked Effect IDs (0x1000 + index)
[[maybe_unused]] static constexpr uint32_t RICHTAP_PREBAKED_CLICK        = 0x1000;
[[maybe_unused]] static constexpr uint32_t RICHTAP_PREBAKED_DOUBLE_CLICK = 0x1001;
[[maybe_unused]] static constexpr uint32_t RICHTAP_PREBAKED_TICK         = 0x1002;
[[maybe_unused]] static constexpr uint32_t RICHTAP_PREBAKED_THUD         = 0x1003;
[[maybe_unused]] static constexpr uint32_t RICHTAP_PREBAKED_POP          = 0x1004;
[[maybe_unused]] static constexpr uint32_t RICHTAP_PREBAKED_HEAVY_CLICK  = 0x1005;

// RichTap Dynamic Duration Transient IDs (0x3001..0x300b)
[[maybe_unused]] static constexpr uint32_t RICHTAP_DYNAMIC_TEXTURE_TICK  = 0x3001;
[[maybe_unused]] static constexpr uint32_t RICHTAP_DYNAMIC_LIGHT_TICK    = 0x3001;
[[maybe_unused]] static constexpr uint32_t RICHTAP_DYNAMIC_TICK          = 0x3002;
[[maybe_unused]] static constexpr uint32_t RICHTAP_DYNAMIC_CLICK         = 0x3003;
[[maybe_unused]] static constexpr uint32_t RICHTAP_DYNAMIC_POP           = 0x3004;
[[maybe_unused]] static constexpr uint32_t RICHTAP_DYNAMIC_HEAVY_CLICK   = 0x3006;

#ifdef USES_OPLUS_AWINIC
#define RICHTAP_OPLUS_ACTIVATE_NODE   "/sys/class/leds/vibrator/oplus_activate"
#define RICHTAP_OPLUS_DURATION_NODE   "/sys/class/leds/vibrator/oplus_duration"
#define RICHTAP_OPLUS_STATE_NODE      "/sys/class/leds/vibrator/oplus_state"
#define RICHTAP_ACTIVATE_NODE         "/sys/class/leds/vibrator/activate"
#define RICHTAP_DURATION_NODE         "/sys/class/leds/vibrator/duration"
#define RICHTAP_STATE_NODE            "/sys/class/leds/vibrator/state"
#define RICHTAP_BRIGHTNESS_NODE       "/sys/class/leds/vibrator/brightness"
#define RICHTAP_VMAX_NODE             "/sys/class/leds/vibrator/vmax"
#define RICHTAP_WAVEFORM_INDEX_NODE   "/sys/class/leds/vibrator/waveform_index"
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
    return access(RICHTAP_OPLUS_ACTIVATE_NODE, F_OK) == 0 ||
           access(RICHTAP_ACTIVATE_NODE, F_OK) == 0;
}

static int sysfsOn(int32_t timeoutMs) {
    if (access(RICHTAP_OPLUS_ACTIVATE_NODE, F_OK) == 0) {
        int ret = writeNode(RICHTAP_OPLUS_STATE_NODE, "1");
        if (ret) {
            return ret;
        }

        ret = writeNode(RICHTAP_OPLUS_DURATION_NODE, timeoutMs);
        if (ret) {
            return ret;
        }

        return writeNode(RICHTAP_OPLUS_ACTIVATE_NODE, "1");
    } else if (access(RICHTAP_ACTIVATE_NODE, F_OK) == 0) {
        int ret = writeNode(RICHTAP_STATE_NODE, "1");
        if (ret) {
            return ret;
        }

        ret = writeNode(RICHTAP_DURATION_NODE, timeoutMs);
        if (ret) {
            return ret;
        }

        return writeNode(RICHTAP_ACTIVATE_NODE, "1");
    }

    return -ENOENT;
}

static int sysfsOff() {
    if (access(RICHTAP_OPLUS_ACTIVATE_NODE, F_OK) == 0) {
        return writeNode(RICHTAP_OPLUS_ACTIVATE_NODE, "0");
    } else if (access(RICHTAP_ACTIVATE_NODE, F_OK) == 0) {
        return writeNode(RICHTAP_ACTIVATE_NODE, "0");
    }

    return -ENOENT;
}

static int sysfsWaveform(int waveformIndex, int vmax) {
    int ret;

    if (access(RICHTAP_BRIGHTNESS_NODE, F_OK) != 0 ||
        access(RICHTAP_WAVEFORM_INDEX_NODE, F_OK) != 0) {
        return -ENOENT;
    }

    if (access(RICHTAP_VMAX_NODE, F_OK) == 0 && vmax > 0) {
        ret = writeNode(RICHTAP_VMAX_NODE, vmax);
        if (ret != 0) {
            return ret;
        }
    }

    ret = writeNode(RICHTAP_WAVEFORM_INDEX_NODE, waveformIndex);
    if (ret != 0) {
        return ret;
    }

    return writeNode(RICHTAP_BRIGHTNESS_NODE, 255);
}
#endif

static int32_t playTactileEffect(uint32_t aacEffectId, int32_t strength,
                                 int hwWaveformIndex __unused, int hwVmax __unused) {
    // 1. Attempt RichTap AAC looper playback
    int32_t dur = aac_vibra_looper_prebaked_effect(aacEffectId, strength);
    if (dur > 0) {
        return dur;
    }

#ifdef USES_OPLUS_AWINIC
    // 2. Fallback to AW8697 native hardware RAM waveform via sysfs
    int ret = sysfsWaveform(hwWaveformIndex, hwVmax);
    if (ret == 0) {
        return 20;
    }

    // 3. Fallback to short sysfs on-pulse
    if (sysfsOn(15) == 0) {
        return 15;
    }
#endif

    return -1;
}

Vibrator::Vibrator() {
    uint32_t deviceType = 0;

#ifdef USES_OPLUS_AWINIC
    mUseSysfsOnOff = hasSysfsOnOffSupport();
    if (mUseSysfsOnOff) {
        ALOGI("Using sysfs backend for on/off and hardware waveforms");
    }
#endif

    int32_t ret = aac_vibra_init(&deviceType);
    if (ret) {
        ALOGE("AAC init failed: %d (hardware sysfs fallback will be active)\n", ret);
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
                    IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS |
                    IVibrator::CAP_GET_RESONANT_FREQUENCY;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    mActiveCompositionId++;

#ifdef USES_OPLUS_AWINIC
    if (mUseSysfsOnOff) {
        int ret = sysfsOff();
        if (ret) {
            ALOGW("Sysfs off failed: %d\n", ret);
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
        ALOGW("AAC off failed: %d\n", ret);
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
    uint64_t myId = mActiveCompositionId;

    // Short tactile feedback pulses (keyboard taps, slider ticks, UI touches)
    // are translated to crisp tactile transients instead of continuous buzzing.
    if (timeoutMs <= 15) {
        int32_t strength = static_cast<int32_t>(mCurrentAmplitude * (60 - 35) + 35);
        strength = std::clamp(strength, 1, 99);
        int hwVmax = static_cast<int>(mCurrentAmplitude * 600 + 600);
        int32_t dur = playTactileEffect(RICHTAP_DYNAMIC_LIGHT_TICK, strength, 2, hwVmax);
        int32_t playMs = (dur > 0) ? dur : timeoutMs;
        if (callback != nullptr) {
            std::thread([this, callback, playMs, myId] {
                usleep(playMs * 1000);
                if (mActiveCompositionId == myId) {
                    callback->onComplete();
                }
            }).detach();
        }
        return ndk::ScopedAStatus::ok();
    }

    if (timeoutMs <= 35) {
        int32_t strength = static_cast<int32_t>(mCurrentAmplitude * (99 - 55) + 55);
        strength = std::clamp(strength, 1, 99);
        int hwVmax = static_cast<int>(mCurrentAmplitude * 1000 + 1500);
        int32_t dur = playTactileEffect(RICHTAP_DYNAMIC_CLICK, strength, 1, hwVmax);
        int32_t playMs = (dur > 0) ? dur : timeoutMs;
        if (callback != nullptr) {
            std::thread([this, callback, playMs, myId] {
                usleep(playMs * 1000);
                if (mActiveCompositionId == myId) {
                    callback->onComplete();
                }
            }).detach();
        }
        return ndk::ScopedAStatus::ok();
    }

    if (timeoutMs <= 50) {
        int32_t strength = static_cast<int32_t>(mCurrentAmplitude * (99 - 60) + 60);
        strength = std::clamp(strength, 1, 99);
        int hwVmax = static_cast<int>(mCurrentAmplitude * 1000 + 1500);
        int32_t dur = playTactileEffect(RICHTAP_DYNAMIC_POP, strength, 3, hwVmax);
        int32_t playMs = (dur > 0) ? dur : timeoutMs;
        if (callback != nullptr) {
            std::thread([this, callback, playMs, myId] {
                usleep(playMs * 1000);
                if (mActiveCompositionId == myId) {
                    callback->onComplete();
                }
            }).detach();
        }
        return ndk::ScopedAStatus::ok();
    }

    if (timeoutMs <= 120) {
        int32_t strength = static_cast<int32_t>(mCurrentAmplitude * (99 - 65) + 65);
        strength = std::clamp(strength, 1, 99);
        int hwVmax = static_cast<int>(mCurrentAmplitude * 1000 + 2000);
        int32_t dur = playTactileEffect(RICHTAP_DYNAMIC_HEAVY_CLICK, strength, 4, hwVmax);
        int32_t playMs = (dur > 0) ? dur : timeoutMs;
        if (callback != nullptr) {
            std::thread([this, callback, playMs, myId] {
                usleep(playMs * 1000);
                if (mActiveCompositionId == myId) {
                    callback->onComplete();
                }
            }).detach();
        }
        return ndk::ScopedAStatus::ok();
    }

    if (timeoutMs <= 250) {
        playTactileEffect(RICHTAP_DYNAMIC_POP, 85, 3, 2200);
        std::thread([this, callback, myId] {
            usleep(45 * 1000);
            if (mActiveCompositionId == myId) {
                playTactileEffect(RICHTAP_DYNAMIC_HEAVY_CLICK, 95, 4, 2500);
            }
            if (callback != nullptr) {
                usleep(30 * 1000);
                if (mActiveCompositionId == myId) {
                    callback->onComplete();
                }
            }
        }).detach();
        return ndk::ScopedAStatus::ok();
    }

    // For sustained long vibrations (>250ms, like incoming phone calls or alarms),
    // use the sysfs backend if available to ensure vibrations never cut off.
#ifdef USES_OPLUS_AWINIC
    if (mUseSysfsOnOff) {
        int ret = sysfsOn(timeoutMs);
        if (ret == 0) {
            if (callback != nullptr) {
                std::thread([this, callback, timeoutMs, myId] {
                    usleep(timeoutMs * 1000);
                    if (mActiveCompositionId == myId) {
                        callback->onComplete();
                    }
                }).detach();
            }
            return ndk::ScopedAStatus::ok();
        }
        ALOGW("sysfsOn failed: %d, falling back to aac_vibra_looper_on", ret);
    }
#endif

    int32_t ret = aac_vibra_looper_on(timeoutMs);
    if (ret < 0) {
#ifdef USES_OPLUS_AWINIC
        if (mUseSysfsOnOff) {
            int sysRet = sysfsOn(timeoutMs);
            if (!sysRet) {
                if (callback != nullptr) {
                    std::thread([this, callback, timeoutMs, myId] {
                        usleep(timeoutMs * 1000);
                        if (mActiveCompositionId == myId) {
                            callback->onComplete();
                        }
                    }).detach();
                }
                return ndk::ScopedAStatus::ok();
            }
        }
#endif
        ALOGE("Both aac_vibra_looper_on and sysfsOn failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([this, callback, ret, myId] {
            usleep(ret * 1000);
            if (mActiveCompositionId == myId) {
                callback->onComplete();
            }
        }).detach();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength es,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
#ifdef OVERRIDE_EFFECTS
    effect = overrideVibratorEffect(effect);
#endif

    mActiveCompositionId++;
    uint64_t myId = mActiveCompositionId;

    uint32_t aacEffectId;
    int32_t strength;
    int hwWaveform;
    int hwVmax;

    switch (effect) {
        case Effect::TEXTURE_TICK:
            aacEffectId = RICHTAP_DYNAMIC_TEXTURE_TICK;
            hwWaveform = 2;
            switch (es) {
                case EffectStrength::LIGHT:
                    strength = 40;
                    hwVmax = 800;
                    break;
                case EffectStrength::MEDIUM:
                    strength = 55;
                    hwVmax = 1000;
                    break;
                case EffectStrength::STRONG:
                default:
                    strength = 70;
                    hwVmax = 1200;
                    break;
            }
            break;

        case Effect::TICK:
            aacEffectId = RICHTAP_DYNAMIC_TICK;
            hwWaveform = 1;
            switch (es) {
                case EffectStrength::LIGHT:
                    strength = 50;
                    hwVmax = 1200;
                    break;
                case EffectStrength::MEDIUM:
                    strength = 65;
                    hwVmax = 1500;
                    break;
                case EffectStrength::STRONG:
                default:
                    strength = 80;
                    hwVmax = 1800;
                    break;
            }
            break;

        case Effect::CLICK:
            aacEffectId = RICHTAP_DYNAMIC_CLICK;
            hwWaveform = 1;
            switch (es) {
                case EffectStrength::LIGHT:
                    strength = RICHTAP_LIGHT_STRENGTH;
                    hwVmax = 1800;
                    break;
                case EffectStrength::MEDIUM:
                    strength = RICHTAP_MEDIUM_STRENGTH;
                    hwVmax = 2200;
                    break;
                case EffectStrength::STRONG:
                default:
                    strength = RICHTAP_STRONG_STRENGTH;
                    hwVmax = 2500;
                    break;
            }
            break;

        case Effect::DOUBLE_CLICK: {
            uint64_t myId = mActiveCompositionId;
            playTactileEffect(RICHTAP_DYNAMIC_CLICK, 99, 1, 2500);
            std::thread([this, callback, myId] {
                usleep(100 * 1000);
                if (mActiveCompositionId != myId) {
                    return;
                }
                playTactileEffect(RICHTAP_DYNAMIC_CLICK, 99, 1, 2500);
                if (callback != nullptr) {
                    usleep(30 * 1000);
                    if (mActiveCompositionId == myId) {
                        callback->onComplete();
                    }
                }
            }).detach();
            if (_aidl_return != nullptr) {
                *_aidl_return = 130;
            }
            return ndk::ScopedAStatus::ok();
        }

        case Effect::POP:
            aacEffectId = RICHTAP_DYNAMIC_POP;
            hwWaveform = 3;
            switch (es) {
                case EffectStrength::LIGHT:
                    strength = 60;
                    hwVmax = 1800;
                    break;
                case EffectStrength::MEDIUM:
                    strength = 75;
                    hwVmax = 2100;
                    break;
                case EffectStrength::STRONG:
                default:
                    strength = 90;
                    hwVmax = 2400;
                    break;
            }
            break;

        case Effect::THUD:
            aacEffectId = RICHTAP_PREBAKED_THUD;
            hwWaveform = 3;
            switch (es) {
                case EffectStrength::LIGHT:
                    strength = 65;
                    hwVmax = 1900;
                    break;
                case EffectStrength::MEDIUM:
                    strength = 80;
                    hwVmax = 2200;
                    break;
                case EffectStrength::STRONG:
                default:
                    strength = 95;
                    hwVmax = 2500;
                    break;
            }
            break;

        case Effect::HEAVY_CLICK:
            aacEffectId = RICHTAP_DYNAMIC_HEAVY_CLICK;
            hwWaveform = 4;
            switch (es) {
                case EffectStrength::LIGHT:
                    strength = 75;
                    hwVmax = 2000;
                    break;
                case EffectStrength::MEDIUM:
                    strength = 89;
                    hwVmax = 2300;
                    break;
                case EffectStrength::STRONG:
                default:
                    strength = 99;
                    hwVmax = 2500;
                    break;
            }
            break;

        default:
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    int32_t ret = playTactileEffect(aacEffectId, strength, hwWaveform, hwVmax);
    if (ret <= 0) {
        ALOGE("playTactileEffect failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([this, callback, ret, myId] {
            usleep(ret * 1000);
            if (mActiveCompositionId == myId) {
                callback->onComplete();
            }
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
            *durationMs = 20;
            break;
        case CompositePrimitive::THUD:
            *durationMs = 25;
            break;
        case CompositePrimitive::SPIN:
            *durationMs = 20;
            break;
        case CompositePrimitive::QUICK_RISE:
            *durationMs = 20;
            break;
        case CompositePrimitive::SLOW_RISE:
            *durationMs = 25;
            break;
        case CompositePrimitive::QUICK_FALL:
            *durationMs = 12;
            break;
        case CompositePrimitive::LIGHT_TICK:
            *durationMs = 5;
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
            if (e.delayMs > 0) {
                if (mActiveCompositionId != myId) {
                    break;
                }
                usleep(e.delayMs * 1000);
            }

            if (mActiveCompositionId != myId) {
                break;
            }

            if (e.primitive == CompositePrimitive::NOOP) {
                continue;
            }

            uint32_t aacId;
            int32_t strength;
            int hwIndex;
            int hwVmax;

            switch (e.primitive) {
                case CompositePrimitive::CLICK:
                    aacId = RICHTAP_DYNAMIC_CLICK;
                    strength = static_cast<int32_t>(e.scale * (99 - 50) + 50);
                    hwIndex = 1;
                    hwVmax = static_cast<int>(e.scale * 1200 + 1300);
                    break;
                case CompositePrimitive::THUD:
                    aacId = RICHTAP_PREBAKED_THUD;
                    strength = static_cast<int32_t>(e.scale * (99 - 50) + 50);
                    hwIndex = 3;
                    hwVmax = static_cast<int>(e.scale * 1200 + 1300);
                    break;
                case CompositePrimitive::SPIN:
                    aacId = RICHTAP_DYNAMIC_POP;
                    strength = static_cast<int32_t>(e.scale * (95 - 45) + 45);
                    hwIndex = 3;
                    hwVmax = static_cast<int>(e.scale * 1000 + 1200);
                    break;
                case CompositePrimitive::QUICK_RISE:
                    aacId = RICHTAP_DYNAMIC_CLICK;
                    strength = static_cast<int32_t>(e.scale * (90 - 45) + 45);
                    hwIndex = 1;
                    hwVmax = static_cast<int>(e.scale * 1000 + 1200);
                    break;
                case CompositePrimitive::SLOW_RISE:
                    aacId = RICHTAP_DYNAMIC_POP;
                    strength = static_cast<int32_t>(e.scale * (90 - 45) + 45);
                    hwIndex = 3;
                    hwVmax = static_cast<int>(e.scale * 1000 + 1200);
                    break;
                case CompositePrimitive::QUICK_FALL:
                    aacId = RICHTAP_DYNAMIC_TICK;
                    strength = static_cast<int32_t>(e.scale * (80 - 40) + 40);
                    hwIndex = 1;
                    hwVmax = static_cast<int>(e.scale * 800 + 1000);
                    break;
                case CompositePrimitive::LIGHT_TICK:
                    aacId = RICHTAP_DYNAMIC_TEXTURE_TICK;
                    strength = static_cast<int32_t>(e.scale * (60 - 30) + 30);
                    hwIndex = 2;
                    hwVmax = static_cast<int>(e.scale * 600 + 600);
                    break;
                case CompositePrimitive::LOW_TICK:
                    aacId = RICHTAP_DYNAMIC_TICK;
                    strength = static_cast<int32_t>(e.scale * (70 - 40) + 40);
                    hwIndex = 1;
                    hwVmax = static_cast<int>(e.scale * 800 + 800);
                    break;
                default:
                    continue;
            }

            strength = std::clamp(strength, 1, 99);
            int32_t dur = playTactileEffect(aacId, strength, hwIndex, hwVmax);
            int32_t waitMs = (dur > 0) ? dur : 12;
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

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz) {
    if (!resonantFreqHz) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    *resonantFreqHz = 170.0f;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor) {
    if (!qFactor) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
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
