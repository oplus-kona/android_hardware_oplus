/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"

#include <algorithm>
#include <chrono>
#include <cutils/properties.h>
#include <fcntl.h>
#include <inttypes.h>
#include <log/log.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef USES_OPLUS_AWINIC
#include <errno.h>
#include <string.h>

#include <string>
#endif
#include <thread>

#include "aac_vibra_function.h"

#ifdef OVERRIDE_EFFECTS
#include "VibratorEffectsOverride.h"
#endif

// The blob maps strength to amplitude as strength/100 of full scale: below 70 it
// plays the level-0 table with gain = strength * 0.02, 70-89 the level-1 table
// with gain = strength * 0.0125, and 90-100 the level-2 table with gain =
// strength * 0.01. The three tables are pre-scaled to peaks of 0.50, 0.80 and
// 1.00 of full scale, so gain is exactly 1.0 at 50/80/100. Those are the
// nominal light/medium/strong amplitudes, matching the sibling qti HAL
// (0x3fff/0x5fff/0x7fff of full scale).
#define RICHTAP_LIGHT_STRENGTH 50
#define RICHTAP_MEDIUM_STRENGTH 80
#define RICHTAP_STRONG_STRENGTH 100

// aac_richtap.config only defines three prebaked effects (0x1000..0x1002) and
// the eleven JND transients 0x3001..0x300b. The durations below are the
// strong-level values; the light/medium levels are 1-3 ms shorter.
static constexpr uint32_t RICHTAP_PREBAKED_DOUBLE_CLICK = 0x1001; // 85 ms
static constexpr uint32_t RICHTAP_JND_THUD               = 0x3001; // 36 ms
static constexpr uint32_t RICHTAP_JND_HEAVY_CLICK        = 0x3002; // 30 ms
static constexpr uint32_t RICHTAP_JND_POP                = 0x3004; // 24 ms
static constexpr uint32_t RICHTAP_JND_CLICK              = 0x3009; // 12 ms
static constexpr uint32_t RICHTAP_JND_TICK               = 0x300a; // 12 ms
static constexpr uint32_t RICHTAP_JND_LIGHT_TICK         = 0x300b; // 11 ms

#define RICHTAP_HAPTIC_DEVICE_NODE "/dev/awinic_haptic"
static constexpr unsigned long RICHTAP_IOCTL_GET_F0 = 0x5211;
static constexpr float RICHTAP_DEFAULT_F0_HZ = 170.0f;

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

// AOSP replaces the requested prebaked strength with the user intensity bucket
// (LOW/MEDIUM/HIGH -> LIGHT/MEDIUM/STRONG), so these are the nominal amplitude
// levels rather than the top of each blob band.
static int32_t strengthForEffectStrength(EffectStrength strength) {
    switch (strength) {
        case EffectStrength::LIGHT:
            return RICHTAP_LIGHT_STRENGTH;
        case EffectStrength::MEDIUM:
            return RICHTAP_MEDIUM_STRENGTH;
        case EffectStrength::STRONG:
        default:
            return RICHTAP_STRONG_STRENGTH;
    }
}

static int32_t primitiveDurationMs(CompositePrimitive primitive) {
    switch (primitive) {
        case CompositePrimitive::NOOP:
            return 0;
        case CompositePrimitive::CLICK:
            return 12;
        case CompositePrimitive::THUD:
            return 36;
        case CompositePrimitive::SPIN:
            return 24;
        case CompositePrimitive::QUICK_RISE:
            return 12;
        case CompositePrimitive::SLOW_RISE:
            return 30;
        case CompositePrimitive::QUICK_FALL:
            return 12;
        case CompositePrimitive::LIGHT_TICK:
            return 11;
        case CompositePrimitive::LOW_TICK:
            return 12;
        default:
            return -1;
    }
}

struct PrimitivePlayback {
    uint32_t aacId;
    int hwWaveform;
    int hwVmax;
};

static PrimitivePlayback primitivePlayback(CompositePrimitive primitive) {
    switch (primitive) {
        case CompositePrimitive::CLICK:
            return {RICHTAP_JND_CLICK, 1, 2200};
        case CompositePrimitive::THUD:
            return {RICHTAP_JND_THUD, 3, 2400};
        case CompositePrimitive::SPIN:
            return {RICHTAP_JND_POP, 3, 2100};
        case CompositePrimitive::QUICK_RISE:
            return {RICHTAP_JND_CLICK, 1, 1800};
        case CompositePrimitive::SLOW_RISE:
            return {RICHTAP_JND_HEAVY_CLICK, 3, 2100};
        case CompositePrimitive::QUICK_FALL:
            return {RICHTAP_JND_TICK, 1, 1500};
        case CompositePrimitive::LIGHT_TICK:
            return {RICHTAP_JND_LIGHT_TICK, 2, 1200};
        case CompositePrimitive::LOW_TICK:
            return {RICHTAP_JND_TICK, 1, 1500};
        default:
            return {0, 0, 0};
    }
}

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
    mAacReady = true;

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

void Vibrator::beginVibration() {
    std::lock_guard<std::mutex> lock(mAmplitudeMutex);
    mPeakAmplitude = mCurrentAmplitude;
    mAmplitudeSetSinceStart = false;

    // Restore the amplitude before starting playback: AOSP calls on() before
    // setAmplitude(), and the previous vibration may have ended in a ramp-down.
    if (mAacReady) {
        aac_vibra_setAmplitude(static_cast<uint8_t>(mCurrentAmplitude * 0xff));
    }
}

void Vibrator::endVibration() {
    std::lock_guard<std::mutex> lock(mAmplitudeMutex);
    if (!mAmplitudeSetSinceStart) {
        return;
    }

    // Only restore the stored value; the gain itself is written when the next
    // vibration starts, so the tail of a ramp-down is not amplified again.
    mCurrentAmplitude = mPeakAmplitude;
    mAmplitudeSetSinceStart = false;
}

void Vibrator::applyFullAmplitude() {
    std::lock_guard<std::mutex> lock(mAmplitudeMutex);

    // Effects carry their own strength/scale, so they must not inherit the gain
    // left behind by a previous amplitude-controlled on() call.
    if (mAacReady) {
        aac_vibra_setAmplitude(0xff);
    }
}

ndk::ScopedAStatus Vibrator::off() {
    mActiveCompositionId++;

#ifdef USES_OPLUS_AWINIC
    if (mUseSysfsOnOff) {
        int ret = sysfsOff();
        if (ret) {
            ALOGW("Sysfs off failed: %d\n", ret);
        }
    }
#endif

    // aac_vibra_off() is a no-op in the blob; aac_vibra_stop() is what issues
    // the RICHTAP_STOP_MODE ioctl that actually cancels playback.
    int32_t stopState = -1;
    int32_t ret = aac_vibra_stop(&stopState);
    if (ret) {
        ALOGW("AAC stop failed: %d\n", ret);
    }

    endVibration();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (timeoutMs <= 0) {
        return ndk::ScopedAStatus::ok();
    }

    uint64_t myId = ++mActiveCompositionId;
    beginVibration();

    // The AAC looper already turns short on() requests into crisp transients
    // (<56 ms) and keeps the full requested duration for longer ones.
    int32_t ret = aac_vibra_looper_on(static_cast<uint32_t>(timeoutMs));
    if (ret <= 0) {
#ifdef USES_OPLUS_AWINIC
        if (mUseSysfsOnOff) {
            int sysRet = sysfsOn(timeoutMs);
            if (sysRet == 0) {
                ret = timeoutMs;
            }
        }
#endif
    }

    if (ret <= 0) {
        ALOGE("Failed to start %dms vibration: %d\n", timeoutMs, ret);
        endVibration();
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([this, callback, ret, myId] {
            usleep(ret * 1000);
            if (mActiveCompositionId == myId) {
                endVibration();
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

    uint32_t aacEffectId;
    int hwWaveform;
    int hwVmax;

    switch (effect) {
        case Effect::TEXTURE_TICK:
            aacEffectId = RICHTAP_JND_LIGHT_TICK;
            hwWaveform = 2;
            hwVmax = 1200;
            break;

        case Effect::TICK:
            aacEffectId = RICHTAP_JND_TICK;
            hwWaveform = 1;
            hwVmax = 1500;
            break;

        case Effect::CLICK:
            aacEffectId = RICHTAP_JND_CLICK;
            hwWaveform = 1;
            hwVmax = 2200;
            break;

        case Effect::DOUBLE_CLICK:
            aacEffectId = RICHTAP_PREBAKED_DOUBLE_CLICK;
            hwWaveform = 1;
            hwVmax = 2200;
            break;

        case Effect::POP:
            aacEffectId = RICHTAP_JND_POP;
            hwWaveform = 3;
            hwVmax = 2100;
            break;

        case Effect::THUD:
            aacEffectId = RICHTAP_JND_THUD;
            hwWaveform = 3;
            hwVmax = 2400;
            break;

        case Effect::HEAVY_CLICK:
            aacEffectId = RICHTAP_JND_HEAVY_CLICK;
            hwWaveform = 4;
            hwVmax = 2500;
            break;

        default:
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    // Only cancel the running vibration once the request is known to be valid.
    uint64_t myId = ++mActiveCompositionId;
    applyFullAmplitude();
    int32_t ret = playTactileEffect(aacEffectId, strengthForEffectStrength(es), hwWaveform,
                                    hwVmax);
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
    // Written as a positive test so that NaN is rejected as well.
    if (!(amplitude > 0.0f && amplitude <= 1.0f)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    std::lock_guard<std::mutex> lock(mAmplitudeMutex);
    mCurrentAmplitude = amplitude;
    if (!mAmplitudeSetSinceStart) {
        mPeakAmplitude = amplitude;
        mAmplitudeSetSinceStart = true;
    } else {
        mPeakAmplitude = std::max(mPeakAmplitude, amplitude);
    }

    int32_t ret = aac_vibra_setAmplitude(static_cast<uint8_t>(amplitude * 0xff));
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
    int32_t duration = primitiveDurationMs(primitive);
    if (duration < 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    *durationMs = duration;
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
        // Written as a positive test so that NaN is rejected as well.
        if (!(e.scale >= 0.0f && e.scale <= 1.0f)) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
        }
        if (std::find(supported.begin(), supported.end(), e.primitive) == supported.end()) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    }

    uint64_t myId = ++mActiveCompositionId;

    std::thread([this, composite, callback, myId]() {
        if (mActiveCompositionId != myId) {
            // Superseded before playback started; keep the newer vibration's gain.
            return;
        }
        applyFullAmplitude();

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

            int32_t durationMs = primitiveDurationMs(e.primitive);
            if (e.scale == 0.0f) {
                // The framework reserves the primitive's duration even when it
                // is muted, so keep the composition timeline intact.
                usleep(durationMs * 1000);
                continue;
            }

            PrimitivePlayback playback = primitivePlayback(e.primitive);
            int32_t strength =
                    std::clamp(static_cast<int32_t>(e.scale * 100.0f + 0.5f), 1, 100);
            int32_t dur = playTactileEffect(playback.aacId, strength, playback.hwWaveform,
                                            playback.hwVmax);
            if (dur <= 0) {
                dur = durationMs;
            }
            usleep(dur * 1000);
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
    float frequency = RICHTAP_DEFAULT_F0_HZ;

    // RICHTAP_GET_F0 returns the calibrated resonant frequency in 0.1 Hz units.
    int fd = open(RICHTAP_HAPTIC_DEVICE_NODE, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        uint32_t f0 = 0;
        if (ioctl(fd, RICHTAP_IOCTL_GET_F0, &f0) == 0 && f0 >= 1000u && f0 <= 3000u) {
            frequency = static_cast<float>(f0) / 10.0f;
        }
        close(fd);
    }

    *resonantFreqHz = frequency;
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
