package com.danmilk.ipodify

import android.content.ComponentName
import android.content.Context
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.service.notification.NotificationListenerService
import android.util.Log

import android.service.notification.StatusBarNotification

class MediaListenerService : NotificationListenerService() {

    companion object {
        private const val TAG = "iPodifyMedia"
        var instance: MediaListenerService? = null
        var lastTrackInfo: String = "Ожидание трека..."
    }

    private var activeController: MediaController? = null
    private val handler = Handler(Looper.getMainLooper())
    private val audioManager by lazy { getSystemService(Context.AUDIO_SERVICE) as AudioManager }

    private var lastPacketSentMs = 0L
    private var lastSentTitle = ""
    private var lastSentPosSec = -1

    private val pollRunnable = object : Runnable {
        override fun run() {
            if (activeController == null) {
                updateActiveController()
            }

            // Если есть токен Spotify, периодически опрашиваем Web API
            if (SpotifyApiHelper.hasValidToken(this@MediaListenerService)) {
                SpotifyApiHelper.getCurrentPlayback(this@MediaListenerService) { info ->
                    if (info != null) {
                        sendSpotifyApiPlayback(info)
                    } else {
                        broadcastCurrentPlayback()
                    }
                }
            } else {
                broadcastCurrentPlayback()
            }

            handler.postDelayed(this, 1000)
        }
    }

    private val callback = object : MediaController.Callback() {
        override fun onPlaybackStateChanged(state: PlaybackState?) {
            broadcastCurrentPlayback()
        }

        override fun onMetadataChanged(metadata: MediaMetadata?) {
            broadcastCurrentPlayback()
        }
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        instance = this
        Log.i(TAG, "Notification / Media Listener connected")
        updateActiveController()
        handler.post(pollRunnable)
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn?.packageName?.contains("spotify") == true) {
            updateActiveController()
            broadcastCurrentPlayback()
        }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification?) {
        if (sbn?.packageName?.contains("spotify") == true) {
            updateActiveController()
        }
    }

    fun updateActiveController() {
        try {
            val sessionManager = getSystemService(Context.MEDIA_SESSION_SERVICE) as MediaSessionManager
            val component = ComponentName(this, MediaListenerService::class.java)
            val controllers = sessionManager.getActiveSessions(component)

            val spotify = controllers.firstOrNull { it.packageName.contains("spotify") }
            val chosen = spotify ?: controllers.firstOrNull()

            if (chosen != activeController) {
                activeController?.unregisterCallback(callback)
                activeController = chosen
                activeController?.registerCallback(callback)
                Log.i(TAG, "Active media controller changed to: ${chosen?.packageName}")
                broadcastCurrentPlayback()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error updating media controller: ${e.message}")
        }
    }

    private fun sendSpotifyApiPlayback(info: SpotifyApiHelper.PlaybackStateInfo) {
        lastTrackInfo = "${info.title} — ${info.artist}"
        MainActivity.instance?.updateUI()

        val curVol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
        val maxVol = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC).coerceAtLeast(1)
        val volPct = (curVol * 100) / maxVol

        val now = System.currentTimeMillis()
        if (info.title == lastSentTitle && info.posSec == lastSentPosSec && (now - lastPacketSentMs < 800)) {
            return
        }

        lastPacketSentMs = now
        lastSentTitle = info.title
        lastSentPosSec = info.posSec

        val shuf = if (info.isShuffled) 1 else 0
        val isPlay = if (info.isPlaying) 1 else 0
        // NP|play|like|shuf|rep|qIdx|qTot|pos|dur|vol|title|artist|album
        val packet = "NP|$isPlay|1|$shuf|${info.repeatState}|1|10|${info.posSec}|${info.durSec}|$volPct|${clean(info.title)}|${clean(info.artist)}|${clean(info.album)}"
        IpodifyBleService.instance?.sendPacketToBle(packet)
    }

    fun broadcastCurrentPlayback() {
        val controller = activeController ?: return
        val metadata = controller.metadata
        val pbState = controller.playbackState

        val isPlaying = pbState?.state == PlaybackState.STATE_PLAYING
        val title = metadata?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = metadata?.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val album = metadata?.getString(MediaMetadata.METADATA_KEY_ALBUM) ?: ""
        val durationMs = metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
        val positionMs = pbState?.position ?: 0L

        if (title.isEmpty()) {
            lastTrackInfo = "Плеер пауза / нет названия"
            MainActivity.instance?.updateUI()
            return
        }

        lastTrackInfo = "$title — $artist"
        MainActivity.instance?.updateUI()

        val posSec = (positionMs / 1000).toInt()
        val durSec = (durationMs / 1000).toInt().coerceAtLeast(1)

        val curVol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
        val maxVol = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC).coerceAtLeast(1)
        val volPct = (curVol * 100) / maxVol

        val now = System.currentTimeMillis()
        if (title == lastSentTitle && posSec == lastSentPosSec && (now - lastPacketSentMs < 800)) {
            return
        }
        if (now - lastPacketSentMs < 250) {
            return
        }

        lastPacketSentMs = now
        lastSentTitle = title
        lastSentPosSec = posSec

        // NP|play|like|shuf|rep|qIdx|qTot|pos|dur|vol|title|artist|album
        val packet = "NP|${if (isPlaying) 1 else 0}|1|0|0|1|10|$posSec|$durSec|$volPct|${clean(title)}|${clean(artist)}|${clean(album)}"
        IpodifyBleService.instance?.sendPacketToBle(packet)
    }

    fun handleCommand(cmd: String) {
        val hasSpotifyToken = SpotifyApiHelper.hasValidToken(this)
        val controller = activeController

        try {
            when {
                cmd == "CMD:PLAY_PAUSE" -> {
                    if (hasSpotifyToken) {
                        val isPlaying = controller?.playbackState?.state == PlaybackState.STATE_PLAYING
                        if (isPlaying) SpotifyApiHelper.pause(this) else SpotifyApiHelper.play(this)
                    }
                    if (controller != null) {
                        val isPlaying = controller.playbackState?.state == PlaybackState.STATE_PLAYING
                        if (isPlaying) controller.transportControls.pause() else controller.transportControls.play()
                    }
                }
                cmd == "CMD:NEXT" -> {
                    if (hasSpotifyToken) SpotifyApiHelper.next(this)
                    controller?.transportControls?.skipToNext()
                }
                cmd == "CMD:PREV" -> {
                    if (hasSpotifyToken) SpotifyApiHelper.previous(this)
                    controller?.transportControls?.skipToPrevious()
                }
                cmd == "CMD:VOL_UP" -> {
                    // Флаг 0 вместо FLAG_SHOW_UI устраняет лавину системных UI-событий
                    audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, 0)
                }
                cmd == "CMD:VOL_DOWN" -> {
                    audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, 0)
                }
                cmd.startsWith("CMD:VOL:") -> {
                    val pct = cmd.substringAfter("CMD:VOL:").toIntOrNull() ?: return
                    val maxVol = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                    val targetVol = (pct * maxVol) / 100
                    audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, targetVol, 0)
                    if (hasSpotifyToken) SpotifyApiHelper.setVolume(this, pct)
                }
                cmd.startsWith("CMD:SEEK:") -> {
                    val sec = cmd.substringAfter("CMD:SEEK:").toLongOrNull() ?: return
                    if (hasSpotifyToken) SpotifyApiHelper.seek(this, sec.toInt())
                    controller?.transportControls?.seekTo(sec * 1000)
                }
                cmd == "CMD:SHUFFLE_ALL" -> {
                    if (hasSpotifyToken) SpotifyApiHelper.shuffle(this, true)
                }
                cmd.startsWith("GET:") -> {
                    val folder = cmd.substringAfter("GET:").substringBefore(":")
                    SpotifyApiHelper.fetchFolderItems(this, folder) { list ->
                        val packet = "MN|$folder|0|${list.joinToString("|")}"
                        IpodifyBleService.instance?.sendPacketToBle(packet)
                    }
                }
                cmd.startsWith("PLAY_ITEM:") -> {
                    val parts = cmd.split(":")
                    val idx = parts.getOrNull(2)?.toIntOrNull() ?: 0
                    val uri = SpotifyApiHelper.playlistUris.getOrNull(idx)
                    if (uri != null) {
                        SpotifyApiHelper.playItem(this, uri)
                    } else {
                        controller?.transportControls?.play()
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Command execution failed: ${e.message}")
        }
    }

    private fun clean(s: String) = s.replace("|", "/")
}
