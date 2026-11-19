package com.danmilk.ipodify

import android.content.ComponentName
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

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
    private var lastArtTrackKey = ""

    @Volatile
    var lastNotificationBitmap: Bitmap? = null

    private var lastSpotifyApiPollMs = 0L
    private var spotifyApiBackoffUntilMs = 0L

    fun resetArtCacheAndResend() {
        lastArtTrackKey = ""
        handler.post {
            broadcastCurrentPlayback(forceArt = true)
        }
    }

    private fun checkAndSendAlbumArt(
        title: String,
        artist: String,
        albumArtUrl: String? = null,
        directBitmap: Bitmap? = null,
        force: Boolean = false
    ) {
        val key = "$title|$artist"
        if (!force && key == lastArtTrackKey && lastArtTrackKey.isNotEmpty()) return
        if (title.isEmpty()) return
        lastArtTrackKey = key

        thread {
            try {
                var bmp: Bitmap? = directBitmap

                if (bmp == null && !albumArtUrl.isNullOrEmpty()) {
                    if (albumArtUrl.startsWith("content://") || albumArtUrl.startsWith("android.resource://") || albumArtUrl.startsWith("file://")) {
                        try {
                            contentResolver.openInputStream(Uri.parse(albumArtUrl))?.use { stream ->
                                bmp = BitmapFactory.decodeStream(stream)
                            }
                        } catch (e: Exception) {
                            Log.w(TAG, "Failed reading content URI: ${e.message}")
                        }
                    } else if (albumArtUrl.startsWith("http://") || albumArtUrl.startsWith("https://")) {
                        try {
                            val conn = URL(albumArtUrl).openConnection() as HttpURLConnection
                            conn.connectTimeout = 4000
                            conn.readTimeout = 4000
                            if (conn.responseCode == 200) {
                                bmp = BitmapFactory.decodeStream(conn.inputStream)
                            }
                            conn.disconnect()
                        } catch (e: Exception) {
                            Log.w(TAG, "Failed downloading album art: ${e.message}")
                        }
                    }
                }

                if (bmp == null && lastNotificationBitmap != null) {
                    bmp = lastNotificationBitmap
                }

                if (bmp != null) {
                    val xbmBytes = AlbumArtProcessor.processTo1BitXbm(bmp!!, 48, 48)
                    val b64 = AlbumArtProcessor.toBase64(xbmBytes)

                    // 1) Full ART: packet (atomic, fits in 512-byte MTU)
                    IpodifyBleService.instance?.sendPacketToBle("ART:$b64")

                    // 2) Also chunked ART0: / ART1: with delay
                    val half = b64.length / 2
                    val part0 = b64.substring(0, half)
                    val part1 = b64.substring(half)
                    Thread.sleep(80)
                    IpodifyBleService.instance?.sendPacketToBle("ART0:$part0")
                    Thread.sleep(80)
                    IpodifyBleService.instance?.sendPacketToBle("ART1:$part1")
                    Log.i(TAG, "Sent 48x48 album art to ESP32 for '$title'")
                } else {
                    Log.w(TAG, "No album art available for '$title'")
                }
            } catch (e: Exception) {
                Log.w(TAG, "Failed to load/send album art: ${e.message}")
            }
        }
    }

    private val pollRunnable = object : Runnable {
        override fun run() {
            if (activeController == null) {
                updateActiveController()
            }

            val now = System.currentTimeMillis()
            // Poll Web API only every 5 seconds to avoid 429 Rate Limits
            if (SpotifyApiHelper.hasValidToken(this@MediaListenerService) &&
                now >= spotifyApiBackoffUntilMs &&
                (now - lastSpotifyApiPollMs >= 5000L)) {
                lastSpotifyApiPollMs = now
                SpotifyApiHelper.getCurrentPlayback(this@MediaListenerService) { info ->
                    if (info != null) {
                        sendSpotifyApiPlayback(info)
                    } else {
                        // Rate limit backoff
                        spotifyApiBackoffUntilMs = System.currentTimeMillis() + 15000L
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
            lastArtTrackKey = ""
            broadcastCurrentPlayback(forceArt = true)
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
        val sbnPkg = sbn?.packageName ?: ""
        if (sbnPkg.contains("spotify") || sbnPkg.contains("music") || activeController?.packageName == sbnPkg) {
            try {
                val notif = sbn?.notification
                val icon = notif?.getLargeIcon()
                if (icon != null) {
                    val drawable = icon.loadDrawable(this)
                    if (drawable is BitmapDrawable) {
                        lastNotificationBitmap = drawable.bitmap
                    } else if (drawable != null) {
                        val bmp = Bitmap.createBitmap(
                            drawable.intrinsicWidth.coerceAtLeast(48),
                            drawable.intrinsicHeight.coerceAtLeast(48),
                            Bitmap.Config.ARGB_8888
                        )
                        val canvas = Canvas(bmp)
                        drawable.setBounds(0, 0, canvas.width, canvas.height)
                        drawable.draw(canvas)
                        lastNotificationBitmap = bmp
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Could not extract notification icon: ${e.message}")
            }
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
        handler.post {
            try {
                val sessionManager = getSystemService(Context.MEDIA_SESSION_SERVICE) as MediaSessionManager
                val component = ComponentName(this, MediaListenerService::class.java)
                val controllers = sessionManager.getActiveSessions(component)

                val spotify = controllers.firstOrNull { it.packageName.contains("spotify") }
                val chosen = spotify ?: controllers.firstOrNull()

                if (chosen != activeController) {
                    try {
                        activeController?.unregisterCallback(callback)
                    } catch (_: Exception) {}
                    activeController = chosen
                    try {
                        activeController?.registerCallback(callback, handler)
                    } catch (e: Exception) {
                        Log.e(TAG, "Failed to register callback: ${e.message}")
                    }
                    Log.i(TAG, "Active media controller changed to: ${chosen?.packageName}")
                    lastArtTrackKey = ""
                    broadcastCurrentPlayback(forceArt = true)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error updating media controller: ${e.message}")
            }
        }
    }

    private fun sendSpotifyApiPlayback(info: SpotifyApiHelper.PlaybackStateInfo) {
        lastTrackInfo = "${info.title} - ${info.artist}"
        MainActivity.instance?.updateUI()

        val curVol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
        val maxVol = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC).coerceAtLeast(1)
        val volPct = (curVol * 100) / maxVol

        val shuf = if (info.isShuffled) 1 else 0
        val isPlay = if (info.isPlaying) 1 else 0
        // NP|play|like|shuf|rep|qIdx|qTot|pos|dur|vol|title|artist|album
        val packet = "NP|$isPlay|1|$shuf|${info.repeatState}|1|10|${info.posSec}|${info.durSec}|$volPct|${clean(info.title)}|${clean(info.artist)}|${clean(info.album)}"
        IpodifyBleService.instance?.sendPacketToBle(packet)

        checkAndSendAlbumArt(info.title, info.artist, albumArtUrl = info.imageUrl)
    }

    fun broadcastCurrentPlayback(forceArt: Boolean = false) {
        val controller = activeController ?: return
        val metadata = controller.metadata
        val pbState = controller.playbackState

        val title = metadata?.getString(MediaMetadata.METADATA_KEY_TITLE)
            ?: metadata?.description?.title?.toString()
            ?: "Unknown Title"
        val artist = metadata?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: metadata?.description?.subtitle?.toString()
            ?: "Unknown Artist"
        val album = metadata?.getString(MediaMetadata.METADATA_KEY_ALBUM)
            ?: metadata?.description?.description?.toString()
            ?: ""

        val durationMs = metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
        val positionMs = pbState?.position ?: 0L
        val isPlaying = pbState?.state == PlaybackState.STATE_PLAYING

        lastTrackInfo = "$title - $artist"
        MainActivity.instance?.updateUI()

        val posSec = (positionMs / 1000).toInt()
        val durSec = (durationMs / 1000).toInt().coerceAtLeast(1)

        val curVol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
        val maxVol = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC).coerceAtLeast(1)
        val volPct = (curVol * 100) / maxVol

        val now = System.currentTimeMillis()
        if (!forceArt && title == lastSentTitle && posSec == lastSentPosSec && (now - lastPacketSentMs < 800)) {
            return
        }
        if (!forceArt && now - lastPacketSentMs < 250) {
            return
        }

        lastPacketSentMs = now
        lastSentTitle = title
        lastSentPosSec = posSec

        // NP|play|like|shuf|rep|qIdx|qTot|pos|dur|vol|title|artist|album
        val packet = "NP|${if (isPlaying) 1 else 0}|1|0|0|1|10|$posSec|$durSec|$volPct|${clean(title)}|${clean(artist)}|${clean(album)}"
        IpodifyBleService.instance?.sendPacketToBle(packet)

        val artUriStr = metadata?.getString(MediaMetadata.METADATA_KEY_ALBUM_ART_URI)
            ?: metadata?.getString(MediaMetadata.METADATA_KEY_ART_URI)
            ?: metadata?.getString(MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI)
            ?: metadata?.description?.iconUri?.toString()

        val directBmp = metadata?.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
            ?: metadata?.description?.iconBitmap
            ?: lastNotificationBitmap

        checkAndSendAlbumArt(title, artist, albumArtUrl = artUriStr, directBitmap = directBmp, force = forceArt)
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
                    lastArtTrackKey = ""
                    if (hasSpotifyToken) SpotifyApiHelper.next(this)
                    controller?.transportControls?.skipToNext()
                }
                cmd == "CMD:PREV" -> {
                    lastArtTrackKey = ""
                    if (hasSpotifyToken) SpotifyApiHelper.previous(this)
                    controller?.transportControls?.skipToPrevious()
                }
                cmd == "CMD:VOL_UP" -> {
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
