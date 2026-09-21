/*
 * SPDX-FileCopyrightText: 2019 CypherOS
 * SPDX-FileCopyrightText: 2014-2020 Paranoid Android
 * SPDX-FileCopyrightText: 2023-2026 The LineageOS Project
 * SPDX-FileCopyrightText: 2023 Yet Another AOSP Project
 * SPDX-FileCopyrightText: 2026 Project ASCP
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.settings.device

import android.animation.Animator
import android.animation.PropertyValuesHolder
import android.animation.ValueAnimator
import android.app.Dialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.TransitionDrawable
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import android.view.CrossWindowBlurListeners
import android.view.Gravity
import android.view.Surface
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import com.android.internal.graphics.drawable.BackgroundBlurDrawable
import java.util.Locale
import java.util.function.Consumer
import android.view.animation.AccelerateInterpolator
import android.view.animation.DecelerateInterpolator
import android.view.animation.OvershootInterpolator
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

class AlertSliderDialog(private val context: Context, private val sysuiContext: Context) :
    Dialog(context, R.style.alert_slider_theme) {
    private val dialogView by lazy { findViewById<LinearLayout>(R.id.alert_slider_dialog)!! }
    private val frameView by lazy { findViewById<ViewGroup>(R.id.alert_slider_view)!! }
    private val iconView by lazy { findViewById<ImageView>(R.id.alert_slider_icon)!! }
    private val textView by lazy { findViewById<TextView>(R.id.alert_slider_text)!! }

    private val mainHandler = Handler(Looper.getMainLooper())
    private var recordingSeconds = 0
    private val recordingTimerRunnable =
        object : Runnable {
            override fun run() {
                recordingSeconds++
                val minutes = recordingSeconds / 60
                val seconds = recordingSeconds % 60
                val label = context.getString(R.string.alert_slider_recording)
                textView.text = String.format(Locale.US, "%s %02d:%02d", label, minutes, seconds)
                mainHandler.postDelayed(this, 1000)
            }
        }

    private val rotation: Int = context.getDisplay().getRotation()
    private val isLandscape = rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270
    private val flip = context.resources.getBoolean(R.bool.alert_slider_dialog_left)

    private val length: Int
    private val xPos: Int
    private val yPos: Int

    private var isAnimating = false
    private var animator = ValueAnimator()

    private var blurDrawable: BackgroundBlurDrawable? = null
    private var isBlurEnabled = false
    private var currentPosition: Int = KeyHandler.POSITION_MIDDLE
    private var currentInvertColors: Boolean = false
    private val blurEnabledListener = Consumer<Boolean> { enabled ->
        isBlurEnabled = enabled
        applyUiTheme(currentInvertColors)
    }

    init {
        window?.let {
            it.requestFeature(Window.FEATURE_NO_TITLE)
            it.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            it.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            it.addFlags(
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH or
                    WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED
            )
            it.addPrivateFlags(WindowManager.LayoutParams.PRIVATE_FLAG_TRUSTED_OVERLAY)
            it.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING)
            it.setType(WindowManager.LayoutParams.TYPE_VOLUME_OVERLAY)
            it.attributes =
                it.attributes.apply {
                    format = PixelFormat.TRANSLUCENT
                    layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    title = TAG
                }
        }

        setCanceledOnTouchOutside(false)
        setContentView(R.layout.alert_slider_dialog)

        val res = context.resources
        val iconWidth = res.getDimensionPixelSize(R.dimen.alert_slider_dialog_icon_width)
        val endPadding = res.getDimensionPixelSize(R.dimen.alert_slider_padding)
        val silent = context.getString(R.string.alert_slider_mode_silent)
        val vibration = context.getString(R.string.alert_slider_mode_vibration)
        val normal = context.getString(R.string.alert_slider_mode_normal)
        val recording = "${context.getString(R.string.alert_slider_recording)} 00:00"
        val maxTextWidth = listOf(silent, vibration, normal, recording).maxOf {
            textView.paint.measureText(it)
        }.toInt()
        val adaptiveWidth = iconWidth + maxTextWidth + endPadding + (res.displayMetrics.density * 4).toInt()

        frameView.layoutParams = frameView.layoutParams.apply {
            width = adaptiveWidth
        }

        val fraction = res.getFraction(R.fraction.alert_slider_dialog_y, 1, 1)
        val widthPixels = res.displayMetrics.widthPixels
        val heightPixels = res.displayMetrics.heightPixels
        val pads = dialogView.paddingTop * 2
        length =
            if (isLandscape) adaptiveWidth
            else res.getDimension(R.dimen.alert_slider_dialog_height).toInt()
        val hv = (length + pads) * 0.5

        xPos =
            if (isLandscape) (widthPixels * fraction - hv).toInt()
            else if (flip) 0 else widthPixels / 100
        yPos =
            if (isLandscape) (if (flip) (widthPixels / 100) else 0)
            else (heightPixels * fraction - hv).toInt()

        window?.let {
            it.attributes =
                it.attributes.apply {
                    gravity =
                        when (rotation) {
                            Surface.ROTATION_0 ->
                                if (flip) Gravity.TOP or Gravity.LEFT
                                else Gravity.TOP or Gravity.RIGHT
                            Surface.ROTATION_90 ->
                                if (flip) Gravity.BOTTOM or Gravity.LEFT
                                else Gravity.TOP or Gravity.LEFT
                            Surface.ROTATION_270 ->
                                if (flip) Gravity.TOP or Gravity.RIGHT
                                else Gravity.BOTTOM or Gravity.RIGHT
                            else ->
                                if (flip) Gravity.BOTTOM or Gravity.LEFT
                                else Gravity.TOP or Gravity.LEFT
                        }

                    x = xPos
                    y = yPos
                }
        }

        if (CrossWindowBlurListeners.CROSS_WINDOW_BLUR_SUPPORTED) {
            dialogView.addOnAttachStateChangeListener(
                object : View.OnAttachStateChangeListener {
                    override fun onViewAttachedToWindow(v: View) {
                        val wm = window?.windowManager
                        isBlurEnabled = wm?.isCrossWindowBlurEnabled ?: false
                        wm?.addCrossWindowBlurEnabledListener(blurEnabledListener)
                        initBlurDrawable(v)
                    }

                    override fun onViewDetachedFromWindow(v: View) {
                        window?.windowManager?.removeCrossWindowBlurEnabledListener(blurEnabledListener)
                        blurDrawable = null
                    }
                }
            )
        }
    }

    @Synchronized
    fun setState(position: Int, ringerMode: Int, invertColors: Boolean) {
        currentPosition = position
        val delta =
            length *
                when (position) {
                    KeyHandler.POSITION_TOP -> -1
                    KeyHandler.POSITION_BOTTOM -> 1
                    else -> 0
                }

        var endX = xPos
        var endY = yPos
        if (isLandscape) endX += delta else endY += delta

        if (isShowing) {
            animatePosition(endX, endY, position, ringerMode, invertColors)
        } else {
            applyUiMode(ringerMode, invertColors)
            applyPositionAndBackground(endX, endY, position)
        }
    }

    @Synchronized
    private fun animatePosition(
        endX: Int,
        endY: Int,
        position: Int,
        ringerMode: Int,
        invertColors: Boolean,
    ) {
        if (isAnimating) animator.cancel()
        animator = ValueAnimator()
        animator.duration = 100
        animator.interpolator = OvershootInterpolator()

        window?.let {
            animator.setValues(
                PropertyValuesHolder.ofInt("x", it.attributes.x, endX),
                PropertyValuesHolder.ofInt("y", it.attributes.y, endY),
            )
        }

        animator.addUpdateListener { animation ->
            window?.let {
                it.attributes =
                    it.attributes.apply {
                        x = animation.getAnimatedValue("x") as Int
                        y = animation.getAnimatedValue("y") as Int
                    }
            }
        }

        animator.addListener(
            object : Animator.AnimatorListener {
                override fun onAnimationStart(animation: Animator) {
                    isAnimating = true
                    currentPosition = position
                    applyUiMode(ringerMode, invertColors)
                    val blur = blurDrawable
                    if (blur != null) {
                        updateBlurCorners(position)
                    } else {
                        val transition =
                            TransitionDrawable(
                                arrayOf(
                                    frameView.background,
                                    context.resources.getDrawable(
                                        backgroundFor(rotation, position, flip),
                                        null,
                                    ),
                                )
                            )
                        frameView.background = transition
                        transition.setCrossFadeEnabled(true)
                        transition.startTransition(30)
                    }
                }

                override fun onAnimationEnd(animation: Animator) {
                    applyPositionAndBackground(endX, endY, position)
                    isAnimating = false
                }

                override fun onAnimationCancel(animation: Animator) {}

                override fun onAnimationRepeat(animation: Animator) {}
            }
        )
        animator.start()
    }

    private fun applyUiTheme(invertColors: Boolean) {
        currentInvertColors = invertColors
        val currentUiMode = sysuiContext.resources.configuration.uiMode
        val isDark =
            (currentUiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK) ==
                android.content.res.Configuration.UI_MODE_NIGHT_YES

        val bgResId =
            if (isDark) {
                android.R.color.system_neutral1_800
            } else {
                android.R.color.system_neutral1_100
            }

        val accentResId =
            if (isDark) {
                android.R.color.system_accent1_100
            } else {
                android.R.color.system_accent1_500
            }

        val bgColor = sysuiContext.getColor(bgResId)
        val accentColor = sysuiContext.getColor(accentResId)

        val blurFgResId =
            if (isDark) {
                android.R.color.system_neutral1_50
            } else {
                android.R.color.system_neutral1_900
            }
        val blurFg = sysuiContext.getColor(blurFgResId)

        val activeFg =
            if (isBlurEnabled) {
                blurFg
            } else if (invertColors) {
                bgColor
            } else {
                accentColor
            }
        val activeBg = if (invertColors) accentColor else bgColor

        textView.setTextColor(activeFg)
        iconView.imageTintList = ColorStateList.valueOf(activeFg)

        val blur = blurDrawable
        if (blur != null) {
            val blurRadius =
                if (isBlurEnabled) {
                    context.resources.getDimensionPixelSize(R.dimen.alert_slider_blur_radius)
                } else {
                    0
                }
            blur.setBlurRadius(blurRadius)
            val pillColor = if (isBlurEnabled) withAlpha(activeBg, BLUR_ALPHA) else activeBg
            blur.setColor(pillColor)
            blur.invalidateSelf()
            frameView.background = blur
            frameView.backgroundTintList = null
            frameView.invalidate()
        } else {
            frameView.backgroundTintList = ColorStateList.valueOf(activeBg)
        }
    }

    private fun applyUiMode(ringerMode: Int, invertColors: Boolean) {
        iconView.setImageResource(
            when (ringerMode) {
                AudioManager.RINGER_MODE_SILENT -> R.drawable.ic_volume_ringer_mute
                AudioManager.RINGER_MODE_VIBRATE -> R.drawable.ic_volume_ringer_vibrate
                AudioManager.RINGER_MODE_NORMAL -> R.drawable.ic_volume_ringer
                KeyHandler.ZEN_PRIORITY_ONLY -> R.drawable.ic_notifications_alert
                KeyHandler.ZEN_TOTAL_SILENCE -> R.drawable.ic_notifications_silence
                KeyHandler.ZEN_ALARMS_ONLY -> R.drawable.ic_alarm
                KeyHandler.TORCH_ON -> R.drawable.ic_torch_on
                KeyHandler.TORCH_OFF -> R.drawable.ic_torch_off
                KeyHandler.RECORD_AUDIO -> R.drawable.ic_mic_record
                else -> R.drawable.ic_info
            }
        )

        mainHandler.removeCallbacks(recordingTimerRunnable)
        if (ringerMode == KeyHandler.RECORD_AUDIO) {
            recordingSeconds = 0
            val label = context.getString(R.string.alert_slider_recording)
            textView.text = String.format(Locale.US, "%s %02d:%02d", label, 0, 0)
            mainHandler.postDelayed(recordingTimerRunnable, 1000)
        } else {
            textView.setText(
                when (ringerMode) {
                    AudioManager.RINGER_MODE_SILENT -> R.string.alert_slider_mode_silent
                    AudioManager.RINGER_MODE_VIBRATE -> R.string.alert_slider_mode_vibration
                    AudioManager.RINGER_MODE_NORMAL -> R.string.alert_slider_mode_normal
                    KeyHandler.ZEN_PRIORITY_ONLY -> R.string.alert_slider_mode_dnd_priority_only
                    KeyHandler.ZEN_TOTAL_SILENCE -> R.string.alert_slider_mode_dnd_total_silence
                    KeyHandler.ZEN_ALARMS_ONLY -> R.string.alert_slider_mode_dnd_alarms_only
                    KeyHandler.TORCH_ON -> R.string.alert_slider_mode_torch_on
                    KeyHandler.TORCH_OFF -> R.string.alert_slider_mode_torch_off
                    else -> R.string.alert_slider_mode_none
                }
            )
        }
        applyUiTheme(invertColors)
    }

    private fun applyPositionAndBackground(endX: Int, endY: Int, position: Int) {
        window?.let {
            it.attributes =
                it.attributes.apply {
                    x = endX
                    y = endY
                }
        }
        currentPosition = position
        val blur = blurDrawable
        if (blur != null) {
            updateBlurCorners(position)
        } else {
            frameView.setBackgroundResource(backgroundFor(rotation, position, flip))
        }
    }

    private fun initBlurDrawable(v: View) {
        if (!CrossWindowBlurListeners.CROSS_WINDOW_BLUR_SUPPORTED) return
        val blur = v.viewRootImpl?.createBackgroundBlurDrawable() ?: return
        blurDrawable = blur
        updateBlurCorners(currentPosition)
        frameView.background = blur
        applyUiTheme(currentInvertColors)
    }

    private fun withAlpha(color: Int, alpha: Int): Int {
        return (color and 0x00FFFFFF) or ((alpha and 0xFF) shl 24)
    }

    private fun updateBlurCorners(position: Int) {
        val blur = blurDrawable ?: return
        val res = context.resources
        val c = res.getDimension(R.dimen.alert_slider_corner_radius)
        val d = res.getDimension(R.dimen.alert_slider_directional_radius)

        var tl = c
        var tr = c
        var bl = c
        var br = c

        when (rotation) {
            Surface.ROTATION_90 -> {
                when (position) {
                    KeyHandler.POSITION_TOP -> if (flip) br = d else tr = d
                    KeyHandler.POSITION_BOTTOM -> if (flip) bl = d else tl = d
                }
            }
            Surface.ROTATION_270 -> {
                when (position) {
                    KeyHandler.POSITION_TOP -> if (flip) tl = d else bl = d
                    KeyHandler.POSITION_BOTTOM -> if (flip) tr = d else br = d
                }
            }
            else -> {
                when (position) {
                    KeyHandler.POSITION_TOP -> if (flip) bl = d else br = d
                    KeyHandler.POSITION_BOTTOM -> if (flip) tl = d else tr = d
                }
            }
        }

        blur.setCornerRadius(tl, tr, bl, br)
    }

    private fun backgroundFor(rotation: Int, position: Int, flip: Boolean): Int {
        fun base(position: Int): Int =
            when (position) {
                KeyHandler.POSITION_TOP ->
                    if (flip) R.drawable.alert_slider_top_flip else R.drawable.alert_slider_top
                KeyHandler.POSITION_MIDDLE -> R.drawable.alert_slider_middle
                KeyHandler.POSITION_BOTTOM ->
                    if (flip) R.drawable.alert_slider_bottom_flip
                    else R.drawable.alert_slider_bottom
                else -> R.drawable.alert_slider_middle
            }

        return when (rotation) {
            Surface.ROTATION_90 ->
                when (position) {
                    KeyHandler.POSITION_TOP ->
                        if (flip) R.drawable.alert_slider_top_90_flip
                        else R.drawable.alert_slider_top_90
                    KeyHandler.POSITION_BOTTOM ->
                        if (flip) R.drawable.alert_slider_bottom_90_flip
                        else R.drawable.alert_slider_bottom_90
                    else -> R.drawable.alert_slider_middle
                }
            Surface.ROTATION_270 ->
                when (position) {
                    KeyHandler.POSITION_TOP ->
                        if (flip) R.drawable.alert_slider_top_270_flip
                        else R.drawable.alert_slider_top_270
                    KeyHandler.POSITION_BOTTOM ->
                        if (flip) R.drawable.alert_slider_bottom_270_flip
                        else R.drawable.alert_slider_bottom_270
                    else -> R.drawable.alert_slider_middle
                }
            else -> base(position)
        }
    }

    override fun show() {
        dialogView.alpha = 0f
        super.show()
        dialogView
            .animate()
            .alpha(1f)
            .setDuration(200)
            .setInterpolator(DecelerateInterpolator())
            .start()
    }

    override fun dismiss() {
        mainHandler.removeCallbacks(recordingTimerRunnable)
        dialogView
            .animate()
            .alpha(0f)
            .setDuration(200)
            .setInterpolator(AccelerateInterpolator())
            .withEndAction { super.dismiss() }
            .start()
    }

    companion object {
        private const val TAG = "AlertSliderDialog"
        private const val BLUR_ALPHA = 80
    }
}
