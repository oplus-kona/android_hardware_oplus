/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aidl/android/hardware/vibrator/BnVibrator.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

class Vibrator : public BnVibrator {
  public:
    Vibrator();
    ~Vibrator() override;

    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* maxDelayMs) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* supported) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive,
                                            int32_t* durationMs) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* resonantFreqHz) override;
    ndk::ScopedAStatus getQFactor(float* qFactor) override;
    ndk::ScopedAStatus getFrequencyResolution(float* freqResolutionHz) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* freqMinimumHz) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* durationMs) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* supported) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;

  private:
    enum class PlaybackPath {
        NONE,
        IOCTL_SRAM,
        SYSFS_SRAM,
        RICHTAP_RTP,
    };

    enum class WorkerJobType {
        NONE,
        CALLBACK_DELAY,
        DOUBLE_CLICK,
        COMPOSE,
    };

    struct WorkerJob {
        uint64_t id = 0;
        WorkerJobType type = WorkerJobType::NONE;
        std::shared_ptr<IVibratorCallback> callback;
        int32_t waitMs = 0;
        bool clearRtpAndEnd = false;
        int hwWaveform = 0;
        int hwVmax = 0;
        int hwGain = 0;
        int32_t hwDurationMs = 0;
        PlaybackPath firstTapPath = PlaybackPath::NONE;
        std::vector<CompositeEffect> composite;
    };

    void beginVibration();
    void endVibration();
    void applyFullAmplitude();
    uint64_t nextCompositionId();
    void submitJob(WorkerJob&& job);
    bool waitInterruptible(uint64_t jobId, int32_t ms);
    void workerLoop();
    int playSramWaveform(int waveformIndex, int vmax, int gain, PlaybackPath* pathOut);
    int32_t playTactileEffect(uint32_t aacEffectId, int32_t strength, int hwWaveformIndex,
                              int hwVmax, int hwGain, int32_t hwDurationMs, bool preferSram,
                              PlaybackPath* pathOut);

    std::atomic<uint64_t> mActiveCompositionId{0};
    std::mutex mAmplitudeMutex;
    float mCurrentAmplitude = 1.0f;
    float mPeakAmplitude = 1.0f;
    bool mAmplitudeSetSinceStart = false;
    bool mAmplitudeSetForEffect = false;
    bool mAacReady = false;
    std::atomic<bool> mRtpActive{false};
    int mHapticFd = -1;
    std::atomic<bool> mIoctlSramSupported{false};
#ifdef USES_OPLUS_AWINIC
    bool mUseSysfsOnOff = false;
    bool mUseSysfsWaveform = false;
    std::atomic<bool> mSysfsActive{false};
#endif

    std::mutex mWorkerMutex;
    std::condition_variable mWorkerCv;
    bool mHasPendingJob = false;
    WorkerJob mPendingJob;
    std::atomic<bool> mWorkerShutdown{false};
    std::thread mWorkerThread;
};

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
