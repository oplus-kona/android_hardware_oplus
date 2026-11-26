# hardware/oplus (Mainline)

Mainline-adapted OPlus hardware HALs, power extension, SystemUI packages, framework stubs, and SELinux policy.

## Layout

- `aidl/livedisplay` — LineageOS LiveDisplay AIDL HAL backed by mainline sysfs (`/sys/kernel/oplus_display/*`)
- `aidl/touch` — LineageOS Touch AIDL HAL backed by mainline procfs/sysfs (`/proc/touchpanel/*`)
- `aidl/vibrator/richtap` — AAC RichTap AIDL V2 Vibrator HAL over `/dev/awinic_haptic`
- `power` — `power-ext-oplus` (`libperfmgr` mode extension for `Mode::DOUBLE_TAP_TO_WAKE`)
- `packages/KeyHandler` — Alert Slider `DeviceKeyHandler` + SystemUI plugin + calibration helper
- `packages/Doze` — `OplusDoze` pickup/pocket ambient display service
- `oplus-fwk` — OPlus framework stubs (`oplus-fwk`)
- `sepolicy/mainline` — Minimal vendor SELinux policy for mainline OPlus components

## Soong options

| Namespace | Variable | Description | Default |
| --------- | -------- | ----------- | ------- |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | ENABLE_AB | Enable AdaptiveBacklight feature | false |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | ENABLE_AF | Enable AntiFlicker feature | false |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | ENABLE_DM | Enable DisplayModes feature | false |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | ENABLE_SE | Enable SunlightEnhancement (HBM) feature | true |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | HBM_NODE | Sysfs node for SunlightEnhancement | /sys/kernel/oplus_display/hbm |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | ANTI_FLICKER_NODE | Sysfs node for AntiFlicker | /sys/kernel/oplus_display/dimlayer_bl_en |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | ADAPTIVE_BACKLIGHT_NODE | Sysfs node for AdaptiveBacklight | /sys/kernel/oplus_display/cabc |
| OPLUS_LINEAGE_LIVEDISPLAY_HAL | DISPLAY_MODES_NODE | Sysfs node for DisplayModes | /sys/kernel/oplus_display/seed |
| OPLUS_LINEAGE_TOUCH_HAL | ENABLE_GM | Enable GloveMode feature | false |
| OPLUS_LINEAGE_TOUCH_HAL | ENABLE_HTPR | Enable HighTouchPollingRate feature | false |
| OPLUS_LINEAGE_TOUCH_HAL | ENABLE_TG | Enable TouchscreenGesture feature | true |
| OPLUS_LINEAGE_TOUCH_HAL | GLOVE_MODE_NODE | Node path for GloveMode | /proc/touchpanel/glove_mode_enable |
| OPLUS_LINEAGE_TOUCH_HAL | HTPR_ENABLE_NODE | Node path for HighTouchPollingRate | /proc/touchpanel/game_switch_enable |
| OPLUS_LINEAGE_TOUCH_HAL | HTPR_ENABLE_VALUE | Value written to enable HighTouchPollingRate | 1 |
| OPLUS_LINEAGE_TOUCH_HAL | GESTURE_ENABLE_INDEP_NODES | Space-separated gesture bitmask node paths | /proc/touchpanel/double_tap_enable_indep |
| OPLUS_LINEAGE_TOUCH_HAL | INCLUDE_DIR | Device-specific include dir path (`TouchscreenGestureConfig.h`) | |
| OPLUS_LINEAGE_VIBRATOR_HAL | INCLUDE_DIR | Device-specific include dir path (`VibratorEffectsOverride.h`) | |
| OPLUS_LINEAGE_VIBRATOR_HAL | OVERRIDE_EFFECTS | Enable device-provided `VibratorEffectsOverride.h` remaps | false |
| OPLUS_LINEAGE_VIBRATOR_HAL | USES_OPLUS_AWINIC | Enable OPlus Awinic sysfs fallback path | false |
