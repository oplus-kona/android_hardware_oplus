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

// Margin added before firing IVibratorCallback::onComplete() for short RTP
// bursts so VibratorManagerService's trailing off() -> RICHTAP_STOP_MODE does
// not cut off the waveform's active braking tail while the async looper thread
// and 400 kHz I2C RTP setup/transfer (~5-10 ms) are still finishing on AW8697.
static constexpr int32_t RICHTAP_RTP_PIPELINE_MARGIN_MS = 15;

// Minimum inter-primitive slack for compose() when primitives are queued back
// to back (delayMs < 8 ms, e.g. SystemUI slider 5x LOW_TICK with pause=0 ms).
// playTactileEffect() dispatches asynchronously to VibratorThread (~6 ms I2C
// setup before RTP playback starts), so without this slack the next primitive's
// RICHTAP_RTP_MODE stop cuts off the previous tick mid-waveform (GLB_ST=0x08).
static constexpr int32_t RICHTAP_COMPOSE_STEP_MARGIN_MS = 8;

// Hardware SRAM playback via RICHTAP_PLAY_SRAM ioctl starts GO=1 synchronously
// before returning (~3 ms total setup, zero LED kworker hop), so only a brief
// active-braking margin is needed.
static constexpr int32_t RICHTAP_IOCTL_SRAM_PIPELINE_MARGIN_MS = 3;
static constexpr int32_t RICHTAP_IOCTL_SRAM_COMPOSE_STEP_MARGIN_MS = 2;

// Fallback sysfs SRAM playback via /sys/class/leds/vibrator/brightness runs on
// the Linux LED classdev set_brightness_delayed kworker (~1 ms wakeup + ~5-6 ms
// I2C setup and 2 ms boost settle before GO=1), verified via I2C ftrace.
static constexpr int32_t RICHTAP_SRAM_PIPELINE_MARGIN_MS = 10;
static constexpr int32_t RICHTAP_SRAM_COMPOSE_STEP_MARGIN_MS = 8;
static constexpr int RICHTAP_SRAM_VMAX_MV = 2500;
static constexpr int RICHTAP_SRAM_MAX_GAIN = 0x80;

#ifdef USES_OPLUS_AWINIC
// Atomic AW8697 F0-locked SRAM playback: arg = wav[6:0] | gain[15:8] | bst_vol[20:16]
static constexpr unsigned long RICHTAP_IOCTL_PLAY_SRAM = 0x5220;
static constexpr uint8_t RICHTAP_SRAM_BST_VOL = 0x16;  // 8.75V boost rail
static constexpr int32_t RICHTAP_SRAM_DOUBLE_CLICK_GAP_MS = 65;
static constexpr int RICHTAP_SRAM_MIN_GAIN = 0x18;
#define RICHTAP_OPLUS_ACTIVATE_NODE   "/sys/class/leds/vibrator/oplus_activate"
#define RICHTAP_OPLUS_DURATION_NODE   "/sys/class/leds/vibrator/oplus_duration"
#define RICHTAP_OPLUS_STATE_NODE      "/sys/class/leds/vibrator/oplus_state"
#define RICHTAP_ACTIVATE_NODE         "/sys/class/leds/vibrator/activate"
#define RICHTAP_DURATION_NODE         "/sys/class/leds/vibrator/duration"
#define RICHTAP_STATE_NODE            "/sys/class/leds/vibrator/state"
#define RICHTAP_BRIGHTNESS_NODE       "/sys/class/leds/vibrator/brightness"
#define RICHTAP_GAIN_NODE             "/sys/class/leds/vibrator/gain"
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

static bool hasSysfsWaveformSupport() {
    return access(RICHTAP_BRIGHTNESS_NODE, W_OK) == 0 &&
           access(RICHTAP_WAVEFORM_INDEX_NODE, W_OK) == 0 &&
           access(RICHTAP_VMAX_NODE, W_OK) == 0;
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
    int ret = -ENOENT;

    if (access(RICHTAP_OPLUS_ACTIVATE_NODE, F_OK) == 0) {
        ret = writeNode(RICHTAP_OPLUS_ACTIVATE_NODE, "0");
    } else if (access(RICHTAP_ACTIVATE_NODE, F_OK) == 0) {
        ret = writeNode(RICHTAP_ACTIVATE_NODE, "0");
    }

    // brightness=0 stops active RAM-mode waveforms (oplus_activate=0 is a no-op
    // when timed-loop state is already 0).
    if (access(RICHTAP_BRIGHTNESS_NODE, W_OK) == 0) {
        int bRet = writeNode(RICHTAP_BRIGHTNESS_NODE, "0");
        if (ret == -ENOENT) {
            ret = bRet;
        }
    }

    return ret;
}

static int sramGainFromScale(float scale) {
    int gain = static_cast<int>(scale * static_cast<float>(RICHTAP_SRAM_MAX_GAIN) + 0.5f);
    return std::clamp(gain, RICHTAP_SRAM_MIN_GAIN, RICHTAP_SRAM_MAX_GAIN);
}

static int sysfsWaveform(int waveformIndex, int vmax, int gain) {
    int ret;

    if (access(RICHTAP_BRIGHTNESS_NODE, W_OK) != 0 ||
        access(RICHTAP_WAVEFORM_INDEX_NODE, W_OK) != 0) {
        return -ENOENT;
    }

    if (access(RICHTAP_VMAX_NODE, W_OK) == 0 && vmax > 0) {
        ret = writeNode(RICHTAP_VMAX_NODE, vmax);
        if (ret != 0) {
            return ret;
        }
    }

    ret = writeNode(RICHTAP_WAVEFORM_INDEX_NODE, waveformIndex);
    if (ret != 0) {
        return ret;
    }

    // Write gain AFTER waveform_index so AW8697 full-scale waveforms (#16-#18)
    // receive the exact requested gain (up to 0x80) instead of being capped.
    if (access(RICHTAP_GAIN_NODE, W_OK) == 0 && gain > 0) {
        ret = writeNode(RICHTAP_GAIN_NODE,
                        std::clamp(gain, RICHTAP_SRAM_MIN_GAIN, RICHTAP_SRAM_MAX_GAIN));
        if (ret != 0) {
            return ret;
        }
    }

    return writeNode(RICHTAP_BRIGHTNESS_NODE, 1);
}
static float scaleForEffectStrength(EffectStrength strength) {
    switch (strength) {
        case EffectStrength::LIGHT:
            return 0.50f;
        case EffectStrength::MEDIUM:
            return 0.80f;
        case EffectStrength::STRONG:
        default:
            return 1.00f;
    }
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

static int32_t primitiveDurationMs(CompositePrimitive primitive, bool useSram) {
    switch (primitive) {
        case CompositePrimitive::NOOP:
            return 0;
        case CompositePrimitive::CLICK:
            return useSram ? 15 : 12;
        case CompositePrimitive::THUD:
            return useSram ? 14 : 36;
        case CompositePrimitive::SPIN:
            return useSram ? 12 : 24;
        case CompositePrimitive::QUICK_RISE:
            return 12;
        case CompositePrimitive::SLOW_RISE:
            return useSram ? 14 : 30;
        case CompositePrimitive::QUICK_FALL:
            return 12;
        case CompositePrimitive::LIGHT_TICK:
            return useSram ? 6 : 11;
        case CompositePrimitive::LOW_TICK:
            return useSram ? 14 : 12;
        default:
            return -1;
    }
}

struct PrimitivePlayback {
    uint32_t aacId;
    int hwWaveform;
    int hwVmax;
    int32_t hwDurationMs;
    float hwGainScale;
};

static PrimitivePlayback primitivePlayback(CompositePrimitive primitive) {
    switch (primitive) {
        case CompositePrimitive::CLICK:
            // Stock OxygenOS SRAM wav #18 (14.6 ms sharp mechanical snap @ 8.75V)
            return {RICHTAP_JND_CLICK, 18, RICHTAP_SRAM_VMAX_MV, 15, 1.00f};
        case CompositePrimitive::THUD:
            // Stock OxygenOS SRAM wav #6 (13.6 ms clean low thud)
            return {RICHTAP_JND_THUD, 6, RICHTAP_SRAM_VMAX_MV, 14, 1.00f};
        case CompositePrimitive::SPIN:
            // Stock OxygenOS SRAM wav #2 (12.0 ms warm punchy pop)
            return {RICHTAP_JND_POP, 2, RICHTAP_SRAM_VMAX_MV, 12, 0.90f};
        case CompositePrimitive::QUICK_RISE:
            // Stock OxygenOS SRAM wav #17 (11.1 ms 180 Hz tight click)
            return {RICHTAP_JND_CLICK, 17, RICHTAP_SRAM_VMAX_MV, 12, 0.85f};
        case CompositePrimitive::SLOW_RISE:
            // Stock OxygenOS SRAM wav #4 (13.2 ms asymmetric heavy snap)
            return {RICHTAP_JND_HEAVY_CLICK, 4, RICHTAP_SRAM_VMAX_MV, 14, 0.95f};
        case CompositePrimitive::QUICK_FALL:
            // Stock OxygenOS SRAM wav #3 (11.1 ms crisp tick)
            return {RICHTAP_JND_TICK, 3, RICHTAP_SRAM_VMAX_MV, 12, 0.85f};
        case CompositePrimitive::LIGHT_TICK:
            // Stock OxygenOS SRAM wav #10 (5.9 ms single-cycle 170 Hz micro-tick)
            return {RICHTAP_JND_LIGHT_TICK, 10, RICHTAP_SRAM_VMAX_MV, 6, 1.00f};
        case CompositePrimitive::LOW_TICK:
            // Stock OxygenOS SRAM wav #1 (13.8 ms 217 Hz smooth slider roller)
            return {RICHTAP_JND_TICK, 1, RICHTAP_SRAM_VMAX_MV, 14, 1.00f};
        default:
            return {0, 0, 0, 0, 0.0f};
    }
}

int Vibrator::playSramWaveform(int waveformIndex, int vmax, int gain, PlaybackPath* pathOut) {
    if (pathOut != nullptr) {
        *pathOut = PlaybackPath::NONE;
    }

#ifdef USES_OPLUS_AWINIC
    if (waveformIndex <= 0) {
        return -EINVAL;
    }

    int clampedGain = std::clamp(gain, RICHTAP_SRAM_MIN_GAIN, RICHTAP_SRAM_MAX_GAIN);

    // 1. Atomic kernel ioctl path: single syscall, synchronous GO=1 (~3 ms,
    // zero sysfs open/close overhead and zero LED kworker hop).
    if (mHapticFd >= 0 && mIoctlSramSupported.load()) {
        unsigned long packed =
                static_cast<unsigned long>(waveformIndex & 0x7f) |
                (static_cast<unsigned long>(clampedGain & 0xff) << 8) |
                (static_cast<unsigned long>(RICHTAP_SRAM_BST_VOL & 0x1f) << 16);
        if (ioctl(mHapticFd, RICHTAP_IOCTL_PLAY_SRAM, packed) == 0) {
            mRtpActive.store(false);
            if (pathOut != nullptr) {
                *pathOut = PlaybackPath::IOCTL_SRAM;
            }
            return 0;
        }
        if (errno == ENOTTY || errno == EOPNOTSUPP) {
            ALOGI("RICHTAP_PLAY_SRAM ioctl unsupported by kernel; falling back to sysfs SRAM");
            mIoctlSramSupported.store(false);
        }
    }

    // 2. Fallback to sysfs SRAM path if kernel lacks RICHTAP_PLAY_SRAM.
    if (mUseSysfsWaveform) {
        if (mRtpActive.exchange(false)) {
            int32_t stopState = -1;
            aac_vibra_stop(&stopState);
        }
        int ret = sysfsWaveform(waveformIndex, vmax, clampedGain);
        if (ret == 0) {
            mSysfsActive.store(true);
            if (pathOut != nullptr) {
                *pathOut = PlaybackPath::SYSFS_SRAM;
            }
            return 0;
        }
        return ret;
    }
#else
    (void)waveformIndex;
    (void)vmax;
    (void)gain;
#endif

    return -ENOENT;
}

int32_t Vibrator::playTactileEffect(uint32_t aacEffectId, int32_t strength, int hwWaveformIndex,
                                    int hwVmax, int hwGain, int32_t hwDurationMs, bool preferSram,
                                    PlaybackPath* pathOut) {
    if (pathOut != nullptr) {
        *pathOut = PlaybackPath::NONE;
    }

#ifdef USES_OPLUS_AWINIC
    // 1. Prefer AW8697 native F0-locked OxygenOS SRAM waveform (atomic ioctl
    // first, sysfs fallback) for crisp tactile transients at 8.75V boost rail.
    if (preferSram && hwWaveformIndex > 0) {
        if (playSramWaveform(hwWaveformIndex, hwVmax, hwGain, pathOut) == 0) {
            return hwDurationMs > 0 ? hwDurationMs : 15;
        }
    }
#else
    (void)hwWaveformIndex;
    (void)hwVmax;
    (void)hwGain;
    (void)hwDurationMs;
    (void)preferSram;
#endif

    // 2. Fallback to RichTap AAC looper playback
    int32_t dur = aac_vibra_looper_prebaked_effect(aacEffectId, strength);
    if (dur > 0) {
        mRtpActive.store(true);
        if (pathOut != nullptr) {
            *pathOut = PlaybackPath::RICHTAP_RTP;
        }
        return dur;
    }

#ifdef USES_OPLUS_AWINIC
    // 3. Fallback to short sysfs on-pulse
    if (sysfsOn(15) == 0) {
        mSysfsActive.store(true);
        if (pathOut != nullptr) {
            *pathOut = PlaybackPath::SYSFS_SRAM;
        }
        return 15;
    }
#endif

    return -1;
}

uint64_t Vibrator::nextCompositionId() {
    uint64_t nextId;
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        nextId = ++mActiveCompositionId;
        mHasPendingJob = false;
        mPendingJob = {};
    }
    mWorkerCv.notify_one();
    return nextId;
}

void Vibrator::submitJob(WorkerJob&& job) {
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        if (mWorkerShutdown.load() || mActiveCompositionId.load() != job.id) {
            return;
        }
        mPendingJob = std::move(job);
        mHasPendingJob = true;
    }
    mWorkerCv.notify_one();
}

bool Vibrator::waitInterruptible(uint64_t jobId, int32_t ms) {
    if (ms <= 0) {
        return !mWorkerShutdown.load() && mActiveCompositionId.load() == jobId;
    }
    std::unique_lock<std::mutex> lock(mWorkerMutex);
    bool interrupted = mWorkerCv.wait_for(lock, std::chrono::milliseconds(ms), [this, jobId]() {
        return mWorkerShutdown.load() || mActiveCompositionId.load() != jobId;
    });
    return !interrupted && !mWorkerShutdown.load() && mActiveCompositionId.load() == jobId;
}

void Vibrator::workerLoop() {
    while (true) {
        WorkerJob job;
        {
            std::unique_lock<std::mutex> lock(mWorkerMutex);
            mWorkerCv.wait(lock, [this]() { return mWorkerShutdown.load() || mHasPendingJob; });
            if (mWorkerShutdown.load()) {
                return;
            }
            job = std::move(mPendingJob);
            mPendingJob = {};
            mHasPendingJob = false;
        }

        if (mActiveCompositionId.load() != job.id) {
            continue;
        }

        switch (job.type) {
            case WorkerJobType::CALLBACK_DELAY: {
                if (waitInterruptible(job.id, job.waitMs)) {
                    if (job.clearRtpAndEnd) {
                        mRtpActive.store(false);
                        endVibration();
                    }
                    if (job.callback != nullptr && mActiveCompositionId.load() == job.id) {
                        job.callback->onComplete();
                    }
                }
                break;
            }

            case WorkerJobType::DOUBLE_CLICK: {
#ifdef USES_OPLUS_AWINIC
                if (!waitInterruptible(job.id, job.hwDurationMs + RICHTAP_SRAM_DOUBLE_CLICK_GAP_MS)) {
                    break;
                }
                PlaybackPath secondPath = PlaybackPath::NONE;
                playSramWaveform(job.hwWaveform, job.hwVmax, job.hwGain, &secondPath);
                int32_t marginMs = (secondPath == PlaybackPath::IOCTL_SRAM)
                                           ? RICHTAP_IOCTL_SRAM_PIPELINE_MARGIN_MS
                                           : RICHTAP_SRAM_PIPELINE_MARGIN_MS;
                if (waitInterruptible(job.id, job.hwDurationMs + marginMs)) {
                    if (job.callback != nullptr && mActiveCompositionId.load() == job.id) {
                        job.callback->onComplete();
                    }
                }
#endif
                break;
            }

            case WorkerJobType::COMPOSE: {
                bool preferSram = false;
#ifdef USES_OPLUS_AWINIC
                preferSram = mIoctlSramSupported.load() || mUseSysfsWaveform;
#endif
                if (!preferSram) {
                    applyFullAmplitude();
                }

                bool playedAny = false;
                PlaybackPath lastPath = PlaybackPath::NONE;
                for (size_t i = 0; i < job.composite.size(); ++i) {
                    const auto& e = job.composite[i];
                    if (e.delayMs > 0 && !waitInterruptible(job.id, e.delayMs)) {
                        break;
                    }
                    if (mActiveCompositionId.load() != job.id) {
                        break;
                    }
                    if (e.primitive == CompositePrimitive::NOOP) {
                        continue;
                    }

                    int32_t durationMs = primitiveDurationMs(e.primitive, preferSram);
                    if (e.scale == 0.0f) {
                        // Reserve the primitive's duration even when muted.
                        if (!waitInterruptible(job.id, durationMs)) {
                            break;
                        }
                        continue;
                    }

                    PrimitivePlayback playback = primitivePlayback(e.primitive);
                    int32_t strength =
                            std::clamp(static_cast<int32_t>(e.scale * 100.0f + 0.5f), 1, 100);
                    int hwGain = RICHTAP_SRAM_MAX_GAIN;
#ifdef USES_OPLUS_AWINIC
                    hwGain = sramGainFromScale(e.scale * playback.hwGainScale);
#endif
                    PlaybackPath path = PlaybackPath::NONE;
                    int32_t dur = playTactileEffect(playback.aacId, strength, playback.hwWaveform,
                                                    playback.hwVmax, hwGain, playback.hwDurationMs,
                                                    preferSram, &path);
                    if (dur <= 0) {
                        dur = durationMs;
                    }
                    playedAny = true;
                    lastPath = path;

                    int32_t stepMarginMs;
                    switch (path) {
                        case PlaybackPath::IOCTL_SRAM:
                            stepMarginMs = RICHTAP_IOCTL_SRAM_COMPOSE_STEP_MARGIN_MS;
                            break;
                        case PlaybackPath::SYSFS_SRAM:
                            stepMarginMs = RICHTAP_SRAM_COMPOSE_STEP_MARGIN_MS;
                            break;
                        default:
                            stepMarginMs = RICHTAP_COMPOSE_STEP_MARGIN_MS;
                            break;
                    }

                    int32_t stepWaitMs = dur;
                    if (i + 1 < job.composite.size()) {
                        int32_t nextDelayMs = job.composite[i + 1].delayMs;
                        if (nextDelayMs < stepMarginMs) {
                            stepWaitMs += (stepMarginMs - nextDelayMs);
                        }
                    }
                    if (!waitInterruptible(job.id, stepWaitMs)) {
                        break;
                    }
                }

                if (playedAny && mActiveCompositionId.load() == job.id) {
                    int32_t tailMarginMs;
                    switch (lastPath) {
                        case PlaybackPath::IOCTL_SRAM:
                            tailMarginMs = RICHTAP_IOCTL_SRAM_PIPELINE_MARGIN_MS;
                            break;
                        case PlaybackPath::SYSFS_SRAM:
                            tailMarginMs = RICHTAP_SRAM_PIPELINE_MARGIN_MS;
                            break;
                        default:
                            tailMarginMs = RICHTAP_RTP_PIPELINE_MARGIN_MS;
                            break;
                    }
                    if (waitInterruptible(job.id, tailMarginMs)) {
                        if (job.callback != nullptr && mActiveCompositionId.load() == job.id) {
                            job.callback->onComplete();
                        }
                    }
                } else if (!playedAny && mActiveCompositionId.load() == job.id &&
                           job.callback != nullptr) {
                    job.callback->onComplete();
                }
                break;
            }

            case WorkerJobType::NONE:
            default:
                break;
        }
    }
}

Vibrator::Vibrator() {
    uint32_t deviceType = 0;

    mHapticFd = open(RICHTAP_HAPTIC_DEVICE_NODE, O_RDWR | O_CLOEXEC);
    if (mHapticFd >= 0) {
#ifdef USES_OPLUS_AWINIC
        mIoctlSramSupported.store(true);
#endif
    } else {
        ALOGW("Failed to open %s: %d", RICHTAP_HAPTIC_DEVICE_NODE, errno);
    }

#ifdef USES_OPLUS_AWINIC
    mUseSysfsOnOff = hasSysfsOnOffSupport();
    mUseSysfsWaveform = hasSysfsWaveformSupport();
    ALOGI("AW8697 backend ready (ioctlSram=%d, sysfsSram=%d, sysfsOnOff=%d)",
          mIoctlSramSupported.load(), mUseSysfsWaveform, mUseSysfsOnOff);
#endif

    mWorkerThread = std::thread(&Vibrator::workerLoop, this);

    int32_t ret = aac_vibra_init(&deviceType);
    if (ret) {
        ALOGE("AAC init failed: %d (hardware SRAM/sysfs backend remains active)\n", ret);
        return;
    }

    aac_vibra_looper_start();
    mAacReady = true;

    ALOGI("AAC init success: %u\n", deviceType);
}

Vibrator::~Vibrator() {
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        mWorkerShutdown.store(true);
        mActiveCompositionId++;
        mHasPendingJob = false;
        mPendingJob = {};
    }
    mWorkerCv.notify_one();
    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }
    if (mHapticFd >= 0) {
        close(mHapticFd);
        mHapticFd = -1;
    }
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
    mAmplitudeSetForEffect = false;

    // Restore the amplitude before starting playback: AOSP calls on() before
    // setAmplitude(), and the previous vibration may have ended in a ramp-down.
    if (mAacReady) {
        aac_vibra_setAmplitude(static_cast<uint8_t>(mCurrentAmplitude * 0xff));
    }
}

void Vibrator::endVibration() {
    std::lock_guard<std::mutex> lock(mAmplitudeMutex);
    mAmplitudeSetForEffect = false;
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
    mAmplitudeSetForEffect = false;

    // Effects carry their own strength/scale, so they must not inherit the gain
    // left behind by a previous amplitude-controlled on() call.
    if (mAacReady) {
        aac_vibra_setAmplitude(0xff);
    }
}

ndk::ScopedAStatus Vibrator::off() {
    nextCompositionId();

#ifdef USES_OPLUS_AWINIC
    if (mSysfsActive.exchange(false) && (mUseSysfsOnOff || mUseSysfsWaveform)) {
        int ret = sysfsOff();
        if (ret) {
            ALOGW("Sysfs off failed: %d\n", ret);
        }
    }
#endif

    // aac_vibra_off() is a no-op in the blob; aac_vibra_stop() is what issues
    // the RICHTAP_STOP_MODE ioctl that actually cancels playback.
    if (mRtpActive.exchange(false) || mAacReady) {
        int32_t stopState = -1;
        int32_t ret = aac_vibra_stop(&stopState);
        if (ret) {
            ALOGW("AAC stop failed: %d\n", ret);
        }
    }

    endVibration();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (timeoutMs <= 0) {
        return ndk::ScopedAStatus::ok();
    }

    uint64_t myId = nextCompositionId();
    beginVibration();

    // The AAC looper already turns short on() requests into crisp transients
    // (<56 ms) and keeps the full requested duration for longer ones.
    int32_t ret = aac_vibra_looper_on(static_cast<uint32_t>(timeoutMs));
    if (ret > 0) {
        mRtpActive.store(true);
    } else {
#ifdef USES_OPLUS_AWINIC
        if (mUseSysfsOnOff) {
            int sysRet = sysfsOn(timeoutMs);
            if (sysRet == 0) {
                mSysfsActive.store(true);
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
        WorkerJob job;
        job.id = myId;
        job.type = WorkerJobType::CALLBACK_DELAY;
        job.callback = callback;
        job.waitMs = ret + (ret < 56 ? RICHTAP_RTP_PIPELINE_MARGIN_MS : 0);
        job.clearRtpAndEnd = true;
        submitJob(std::move(job));
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
    int hwVmax = RICHTAP_SRAM_VMAX_MV;
    int32_t hwDurationMs;
    [[maybe_unused]] float hwGainScale = 1.0f;
    [[maybe_unused]] bool isDoubleClick = false;

    switch (effect) {
        case Effect::TEXTURE_TICK:
            aacEffectId = RICHTAP_JND_LIGHT_TICK;
            hwWaveform = 10;
            hwDurationMs = 6;
            break;

        case Effect::TICK:
            aacEffectId = RICHTAP_JND_TICK;
            hwWaveform = 3;
            hwDurationMs = 12;
            break;

        case Effect::CLICK:
            aacEffectId = RICHTAP_JND_CLICK;
            hwWaveform = 18;
            hwDurationMs = 15;
            break;

        case Effect::DOUBLE_CLICK:
            aacEffectId = RICHTAP_PREBAKED_DOUBLE_CLICK;
            hwWaveform = 17;
            hwDurationMs = 12;
            isDoubleClick = true;
            break;

        case Effect::POP:
            aacEffectId = RICHTAP_JND_POP;
            hwWaveform = 2;
            hwDurationMs = 12;
            break;

        case Effect::THUD:
            aacEffectId = RICHTAP_JND_THUD;
            hwWaveform = 6;
            hwDurationMs = 14;
            break;

        case Effect::HEAVY_CLICK:
            aacEffectId = RICHTAP_JND_HEAVY_CLICK;
            hwWaveform = 4;
            hwDurationMs = 14;
            break;

        default:
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    // Only cancel the running vibration once the request is known to be valid.
    uint64_t myId = nextCompositionId();

    // A framework-provided amplitude applies to this effect only. beginVibration()
    // keeps it as the current gain, while applyFullAmplitude() restores the
    // pre-percent behavior when no amplitude was set for this effect.
    bool explicitAmplitude;
    [[maybe_unused]] float effectAmplitude = 1.0f;
    {
        std::lock_guard<std::mutex> lock(mAmplitudeMutex);
        explicitAmplitude = mAmplitudeSetForEffect;
        effectAmplitude = explicitAmplitude ? mCurrentAmplitude : 1.0f;
        mAmplitudeSetForEffect = false;
    }

    bool preferSram = false;
#ifdef USES_OPLUS_AWINIC
    preferSram = mIoctlSramSupported.load() || mUseSysfsWaveform;
#endif

    if (!preferSram) {
        if (explicitAmplitude) {
            beginVibration();
        } else {
            applyFullAmplitude();
        }
    }

    int hwGain = RICHTAP_SRAM_MAX_GAIN;
#ifdef USES_OPLUS_AWINIC
    hwGain = sramGainFromScale(scaleForEffectStrength(es) * hwGainScale * effectAmplitude);
#endif

    PlaybackPath path = PlaybackPath::NONE;
    int32_t ret = playTactileEffect(aacEffectId, strengthForEffectStrength(es), hwWaveform,
                                    hwVmax, hwGain, hwDurationMs, preferSram, &path);
    if (ret <= 0) {
        ALOGE("playTactileEffect failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

#ifdef USES_OPLUS_AWINIC
    bool usedSram = (path == PlaybackPath::IOCTL_SRAM || path == PlaybackPath::SYSFS_SRAM);
    if (usedSram && isDoubleClick) {
        int32_t totalDurationMs = hwDurationMs * 2 + RICHTAP_SRAM_DOUBLE_CLICK_GAP_MS;
        WorkerJob job;
        job.id = myId;
        job.type = WorkerJobType::DOUBLE_CLICK;
        job.callback = callback;
        job.hwWaveform = hwWaveform;
        job.hwVmax = hwVmax;
        job.hwGain = hwGain;
        job.hwDurationMs = hwDurationMs;
        job.firstTapPath = path;
        submitJob(std::move(job));

        if (_aidl_return != nullptr) {
            *_aidl_return = totalDurationMs;
        }
        return ndk::ScopedAStatus::ok();
    }
#endif

    if (callback != nullptr) {
        int32_t marginMs;
        switch (path) {
            case PlaybackPath::IOCTL_SRAM:
                marginMs = RICHTAP_IOCTL_SRAM_PIPELINE_MARGIN_MS;
                break;
            case PlaybackPath::SYSFS_SRAM:
                marginMs = RICHTAP_SRAM_PIPELINE_MARGIN_MS;
                break;
            default:
                marginMs = RICHTAP_RTP_PIPELINE_MARGIN_MS;
                break;
        }
        WorkerJob job;
        job.id = myId;
        job.type = WorkerJobType::CALLBACK_DELAY;
        job.callback = callback;
        job.waitMs = ret + marginMs;
        submitJob(std::move(job));
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
    mAmplitudeSetForEffect = true;
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
    bool useSram = false;
#ifdef USES_OPLUS_AWINIC
    useSram = mIoctlSramSupported.load() || mUseSysfsWaveform;
#endif
    int32_t duration = primitiveDurationMs(primitive, useSram);
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

    uint64_t myId = nextCompositionId();
    WorkerJob job;
    job.id = myId;
    job.type = WorkerJobType::COMPOSE;
    job.callback = callback;
    job.composite = composite;
    submitJob(std::move(job));

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
    int fd = mHapticFd;
    bool ownedFd = false;
    if (fd < 0) {
        fd = open(RICHTAP_HAPTIC_DEVICE_NODE, O_RDWR | O_CLOEXEC);
        ownedFd = (fd >= 0);
    }
    if (fd >= 0) {
        uint32_t f0 = 0;
        if (ioctl(fd, RICHTAP_IOCTL_GET_F0, &f0) == 0 && f0 >= 1000u && f0 <= 3000u) {
            frequency = static_cast<float>(f0) / 10.0f;
        }
        if (ownedFd) {
            close(fd);
        }
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
