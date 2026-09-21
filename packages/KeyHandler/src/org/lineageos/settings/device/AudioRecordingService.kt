/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.settings.device

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.MediaRecorder
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Environment
import android.os.IBinder
import android.util.Log
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class AudioRecordingService : Service() {
    private var mediaRecorder: MediaRecorder? = null
    private var currentOutputFile: File? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_RECORDING -> startRecording()
            ACTION_STOP_RECORDING -> stopRecording()
        }
        return START_NOT_STICKY
    }

    private fun startRecording() {
        if (isRecording) return

        try {
            createNotificationChannel()
            val notification = buildNotification()

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(
                    NOTIFICATION_ID,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
                )
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }

            val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_RECORDINGS)
            if (!dir.exists()) {
                dir.mkdirs()
            }
            val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
            val file = File(dir, "AlertSlider_$timestamp.m4a")
            currentOutputFile = file

            val recorder =
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    MediaRecorder(this)
                } else {
                    @Suppress("DEPRECATION")
                    MediaRecorder()
                }

            recorder.apply {
                setAudioSource(MediaRecorder.AudioSource.MIC)
                setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
                setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
                setAudioEncodingBitRate(192000)
                setAudioSamplingRate(48000)
                setOutputFile(file.absolutePath)
                prepare()
                start()
            }

            mediaRecorder = recorder
            isRecording = true
            Log.d(TAG, "Recording started: ${file.absolutePath}")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start recording", e)
            stopRecording()
        }
    }

    private fun stopRecording() {
        try {
            mediaRecorder?.apply {
                try {
                    stop()
                } catch (e: Exception) {
                    Log.e(TAG, "Error stopping recorder", e)
                }
                release()
            }
            mediaRecorder = null

            currentOutputFile?.let { file ->
                if (file.exists() && file.length() > 0) {
                    MediaScannerConnection.scanFile(
                        applicationContext,
                        arrayOf(file.absolutePath),
                        arrayOf("audio/mp4", "audio/m4a"),
                        null,
                    )
                    Log.d(TAG, "Recording saved: ${file.absolutePath} (${file.length()} bytes)")
                } else if (file.exists()) {
                    file.delete()
                }
            }
            currentOutputFile = null
        } catch (e: Exception) {
            Log.e(TAG, "Error in stopRecording", e)
        } finally {
            isRecording = false
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    private fun createNotificationChannel() {
        val channel =
            NotificationChannel(
                CHANNEL_ID,
                getString(R.string.alert_slider_recording_channel_name),
                NotificationManager.IMPORTANCE_LOW,
            ).apply {
                description = getString(R.string.alert_slider_recording_notification_title)
            }
        val nm = getSystemService(NotificationManager::class.java)
        nm?.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val stopIntent =
            Intent(this, AudioRecordingService::class.java).apply {
                action = ACTION_STOP_RECORDING
            }
        val stopPendingIntent =
            PendingIntent.getService(
                this,
                0,
                stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.alert_slider_recording_notification_title))
            .setContentText(getString(R.string.alert_slider_recording_notification_text))
            .setSmallIcon(R.drawable.ic_mic_record)
            .addAction(
                Notification.Action.Builder(
                    null,
                    getString(R.string.alert_slider_recording_stop),
                    stopPendingIntent,
                ).build(),
            )
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        stopRecording()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "AudioRecordingService"
        private const val CHANNEL_ID = "alert_slider_recording"
        private const val NOTIFICATION_ID = 1001

        const val ACTION_START_RECORDING = "org.lineageos.settings.device.action.START_RECORDING"
        const val ACTION_STOP_RECORDING = "org.lineageos.settings.device.action.STOP_RECORDING"

        var isRecording = false
            private set

        fun start(context: Context) {
            val intent =
                Intent(context, AudioRecordingService::class.java).apply {
                    action = ACTION_START_RECORDING
                }
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            val intent =
                Intent(context, AudioRecordingService::class.java).apply {
                    action = ACTION_STOP_RECORDING
                }
            context.startService(intent)
        }
    }
}
