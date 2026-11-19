package com.danmilk.ipodify

import android.content.Context
import android.util.Base64
import android.util.Log
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import kotlin.concurrent.thread

object SpotifyApiHelper {
    private const val TAG = "SpotifyApiHelper"
    const val DEFAULT_CLIENT_ID = "49be876031464f1ba5a139eb691a4ef5"
    const val REDIRECT_URI = "ipodify://callback"

    var containerUris = mutableListOf<String>()
    var trackUris = mutableListOf<String>()
    var playlistUris = trackUris

    data class PlaybackStateInfo(
        val isPlaying: Boolean,
        val title: String,
        val artist: String,
        val album: String,
        val posSec: Int,
        val durSec: Int,
        val volumePct: Int,
        val isShuffled: Boolean,
        val repeatState: Int, // 0: off, 1: context, 2: track
        val imageUrl: String? = null
    )

    fun getClientId(context: Context): String {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        return prefs.getString("spotify_client_id", DEFAULT_CLIENT_ID) ?: DEFAULT_CLIENT_ID
    }

    fun saveClientId(context: Context, clientId: String) {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        prefs.edit().putString("spotify_client_id", clientId.trim()).apply()
    }

    fun getAccessToken(context: Context): String? {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        return prefs.getString("spotify_access_token", null)
    }

    fun saveAccessToken(context: Context, token: String) {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        prefs.edit().putString("spotify_access_token", token.trim()).apply()
    }

    fun getRefreshToken(context: Context): String? {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        return prefs.getString("spotify_refresh_token", null)
    }

    fun saveRefreshToken(context: Context, token: String) {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        prefs.edit().putString("spotify_refresh_token", token.trim()).apply()
    }

    fun hasValidToken(context: Context): Boolean {
        val token = getAccessToken(context)
        return !token.isNullOrEmpty()
    }

    // ==================== PKCE AUTHENTICATION ====================

    private fun generateCodeVerifier(): String {
        val secureRandom = SecureRandom()
        val code = ByteArray(32)
        secureRandom.nextBytes(code)
        return Base64.encodeToString(code, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
    }

    private fun generateCodeChallenge(verifier: String): String {
        val bytes = verifier.toByteArray(Charsets.US_ASCII)
        val md = MessageDigest.getInstance("SHA-256")
        val digest = md.digest(bytes)
        return Base64.encodeToString(digest, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
    }

    fun getAuthorizeUrl(context: Context): String {
        val clientId = getClientId(context)
        val verifier = generateCodeVerifier()
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        prefs.edit().putString("spotify_code_verifier", verifier).apply()

        val challenge = generateCodeChallenge(verifier)
        val scope = "playlist-read-private%20playlist-read-collaborative%20user-read-playback-state%20user-modify-playback-state%20user-library-read%20user-read-recently-played%20user-top-read%20user-follow-read"
        val encRedirect = URLEncoder.encode(REDIRECT_URI, "UTF-8")

        return "https://accounts.spotify.com/authorize?client_id=$clientId&response_type=code&redirect_uri=$encRedirect&code_challenge_method=S256&code_challenge=$challenge&scope=$scope"
    }

    fun exchangeCodeForToken(context: Context, code: String, callback: (Boolean, String) -> Unit) {
        val clientId = getClientId(context)
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        val verifier = prefs.getString("spotify_code_verifier", null)

        if (verifier.isNullOrEmpty()) {
            callback(false, "Ошибка: code_verifier не найден")
            return
        }

        thread {
            var conn: HttpURLConnection? = null
            try {
                val url = URL("https://accounts.spotify.com/api/token")
                conn = url.openConnection() as HttpURLConnection
                conn.requestMethod = "POST"
                conn.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
                conn.connectTimeout = 6000
                conn.readTimeout = 6000
                conn.doOutput = true

                val encRedirect = URLEncoder.encode(REDIRECT_URI, "UTF-8")
                val encCode = URLEncoder.encode(code, "UTF-8")
                val encVerifier = URLEncoder.encode(verifier, "UTF-8")
                val postParams = "client_id=$clientId&grant_type=authorization_code&code=$encCode&redirect_uri=$encRedirect&code_verifier=$encVerifier"
                val writer = OutputStreamWriter(conn.outputStream)
                writer.write(postParams)
                writer.flush()
                writer.close()

                val resCode = conn.responseCode
                if (resCode in 200..299) {
                    val reader = BufferedReader(InputStreamReader(conn.inputStream))
                    val body = reader.readText()
                    reader.close()

                    val json = JSONObject(body)
                    val accessToken = json.optString("access_token")
                    val refreshToken = json.optString("refresh_token")

                    if (accessToken.isNotEmpty()) {
                        saveAccessToken(context, accessToken)
                        if (refreshToken.isNotEmpty()) {
                            saveRefreshToken(context, refreshToken)
                        }
                        callback(true, "Spotify успешно авторизован!")
                        return@thread
                    }
                }

                val errReader = conn.errorStream?.let { BufferedReader(InputStreamReader(it)) }
                val errBody = errReader?.readText() ?: "HTTP $resCode"
                errReader?.close()
                Log.e(TAG, "Token exchange failed: $errBody")
                callback(false, "Ошибка Spotify: $errBody")
            } catch (e: Exception) {
                Log.e(TAG, "Token exchange exception: ${e.message}")
                callback(false, "Ошибка сети: ${e.message}")
            } finally {
                conn?.disconnect()
            }
        }
    }

    fun refreshAccessToken(context: Context, callback: ((Boolean) -> Unit)? = null) {
        val refreshToken = getRefreshToken(context)
        val clientId = getClientId(context)
        if (refreshToken.isNullOrEmpty()) {
            callback?.invoke(false)
            return
        }

        thread {
            var conn: HttpURLConnection? = null
            try {
                val url = URL("https://accounts.spotify.com/api/token")
                conn = url.openConnection() as HttpURLConnection
                conn.requestMethod = "POST"
                conn.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
                conn.connectTimeout = 6000
                conn.readTimeout = 6000
                conn.doOutput = true

                val encRefresh = URLEncoder.encode(refreshToken, "UTF-8")
                val postParams = "client_id=$clientId&grant_type=refresh_token&refresh_token=$encRefresh"
                val writer = OutputStreamWriter(conn.outputStream)
                writer.write(postParams)
                writer.flush()
                writer.close()

                if (conn.responseCode in 200..299) {
                    val reader = BufferedReader(InputStreamReader(conn.inputStream))
                    val body = reader.readText()
                    reader.close()

                    val json = JSONObject(body)
                    val accessToken = json.optString("access_token")
                    val newRefresh = json.optString("refresh_token")
                    if (accessToken.isNotEmpty()) {
                        saveAccessToken(context, accessToken)
                        if (newRefresh.isNotEmpty()) saveRefreshToken(context, newRefresh)
                        callback?.invoke(true)
                        return@thread
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to refresh token: ${e.message}")
            } finally {
                conn?.disconnect()
            }
            callback?.invoke(false)
        }
    }

    // ==================== GENERIC API REQUEST ====================

    private fun apiRequest(
        context: Context,
        endpoint: String,
        method: String = "GET",
        bodyJson: String? = null,
        callback: ((Int, String?) -> Unit)? = null
    ) {
        val token = getAccessToken(context)
        if (token.isNullOrEmpty()) {
            callback?.invoke(401, null)
            return
        }

        thread {
            var conn: HttpURLConnection? = null
            try {
                val url = URL(if (endpoint.startsWith("http")) endpoint else "https://api.spotify.com/v1$endpoint")
                conn = url.openConnection() as HttpURLConnection
                conn.requestMethod = method
                conn.setRequestProperty("Authorization", "Bearer $token")
                conn.setRequestProperty("Content-Type", "application/json")
                conn.connectTimeout = 3500
                conn.readTimeout = 3500

                if (bodyJson != null && (method == "POST" || method == "PUT")) {
                    conn.doOutput = true
                    val writer = OutputStreamWriter(conn.outputStream)
                    writer.write(bodyJson)
                    writer.flush()
                    writer.close()
                }

                val code = conn.responseCode
                if (code == 401) {
                    // Токен устарел — пробуем обновить через refresh_token
                    refreshAccessToken(context)
                }

                val responseStr = if (code in 200..299) {
                    val reader = BufferedReader(InputStreamReader(conn.inputStream))
                    val str = reader.readText()
                    reader.close()
                    str
                } else null

                callback?.invoke(code, responseStr)
            } catch (e: Exception) {
                Log.e(TAG, "API request $method $endpoint failed: ${e.message}")
                callback?.invoke(500, null)
            } finally {
                conn?.disconnect()
            }
        }
    }

    // ==================== PLAYER CONTROLS ====================

    fun play(context: Context) {
        apiRequest(context, "/me/player/play", "PUT")
    }

    fun pause(context: Context) {
        apiRequest(context, "/me/player/pause", "PUT")
    }

    fun next(context: Context) {
        apiRequest(context, "/me/player/next", "POST")
    }

    fun previous(context: Context) {
        apiRequest(context, "/me/player/previous", "POST")
    }

    fun seek(context: Context, posSec: Int) {
        val ms = posSec * 1000
        apiRequest(context, "/me/player/seek?position_ms=$ms", "PUT")
    }

    fun setVolume(context: Context, pct: Int) {
        val clamped = pct.coerceIn(0, 100)
        apiRequest(context, "/me/player/volume?volume_percent=$clamped", "PUT")
    }

    fun shuffle(context: Context, state: Boolean) {
        apiRequest(context, "/me/player/shuffle?state=$state", "PUT")
    }

    fun getCurrentPlayback(context: Context, callback: (PlaybackStateInfo?) -> Unit) {
        apiRequest(context, "/me/player", "GET") { code, body ->
            if (code == 200 && !body.isNullOrEmpty()) {
                try {
                    val json = JSONObject(body)
                    val isPlaying = json.optBoolean("is_playing", false)
                    val progressMs = json.optLong("progress_ms", 0L)
                    val shuffleState = json.optBoolean("shuffle_state", false)
                    val repStr = json.optString("repeat_state", "off")
                    val repMode = when (repStr) {
                        "track" -> 2
                        "context" -> 1
                        else -> 0
                    }

                    val device = json.optJSONObject("device")
                    val volPct = device?.optInt("volume_percent", 65) ?: 65

                    val item = json.optJSONObject("item")
                    if (item != null) {
                        val title = item.optString("name", "Unknown Title")
                        val durMs = item.optLong("duration_ms", 0L)
                        val albumObj = item.optJSONObject("album")
                        val album = albumObj?.optString("name", "") ?: ""

                        val artistsArr = item.optJSONArray("artists")
                        val artist = if (artistsArr != null && artistsArr.length() > 0) {
                            artistsArr.getJSONObject(0).optString("name", "Unknown Artist")
                        } else ""

                        val imagesArr = albumObj?.optJSONArray("images")
                        val imgUrl = if (imagesArr != null && imagesArr.length() > 0) {
                            val idx = if (imagesArr.length() > 1) 1 else 0
                            imagesArr.getJSONObject(idx).optString("url", "")
                        } else null

                        val info = PlaybackStateInfo(
                            isPlaying = isPlaying,
                            title = title,
                            artist = artist,
                            album = album,
                            posSec = (progressMs / 1000).toInt(),
                            durSec = (durMs / 1000).toInt().coerceAtLeast(1),
                            volumePct = volPct,
                            isShuffled = shuffleState,
                            repeatState = repMode,
                            imageUrl = imgUrl
                        )
                        callback(info)
                        return@apiRequest
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error parsing /me/player: ${e.message}")
                }
            }
            callback(null)
        }
    }

    // ==================== FOLDERS & PLAYLISTS ====================

    fun fetchFolderItems(context: Context, folder: String, callback: (List<String>) -> Unit) {
        val endpoint: String? = when {
            folder == "playlists" -> "/me/playlists?limit=30"
            folder.startsWith("pl_tracks:") -> {
                val idx = folder.substringAfter("pl_tracks:").toIntOrNull() ?: 0
                val pUri = containerUris.getOrNull(idx)
                val id = pUri?.substringAfter("playlist:")?.substringBefore("?")
                if (id != null) "/playlists//tracks?limit=35" else null
            }
            folder == "artists" -> "/me/following?type=artist&limit=30"
            folder.startsWith("art_tracks:") -> {
                val idx = folder.substringAfter("art_tracks:").toIntOrNull() ?: 0
                val aUri = containerUris.getOrNull(idx)
                val id = aUri?.substringAfter("artist:")?.substringBefore("?")
                if (id != null) "/artists//top-tracks?market=from_token" else null
            }
            folder == "albums" -> "/me/albums?limit=30"
            folder.startsWith("alb_tracks:") -> {
                val idx = folder.substringAfter("alb_tracks:").toIntOrNull() ?: 0
                val aUri = containerUris.getOrNull(idx)
                val id = aUri?.substringAfter("album:")?.substringBefore("?")
                if (id != null) "/albums//tracks?limit=35" else null
            }
            folder == "recent" -> "/me/player/recently-played?limit=30"
            folder == "queue" -> "/me/player/queue"
            folder == "songs" || folder == "top" -> "/me/tracks?limit=35"
            else -> "/me/tracks?limit=30"
        }

        if (endpoint == null) {
            callback(listOf("РџСѓСЃС‚Рѕ"))
            return
        }

        apiRequest(context, endpoint, "GET") { code, body ->
            if (code == 200 && !body.isNullOrEmpty()) {
                try {
                    val json = JSONObject(body)
                    val names = mutableListOf<String>()

                    when {
                        folder == "playlists" || folder == "artists" || folder == "albums" -> {
                            containerUris.clear()
                            val items = if (folder == "artists") {
                                json.optJSONObject("artists")?.optJSONArray("items")
                            } else {
                                json.optJSONArray("items")
                            }
                            if (items != null) {
                                for (i in 0 until items.length()) {
                                    val obj = items.getJSONObject(i)
                                    val (name, uri) = when (folder) {
                                        "playlists", "artists" -> Pair(obj.optString("name"), obj.optString("uri"))
                                        "albums" -> {
                                            val alb = obj.optJSONObject("album") ?: obj
                                            Pair(alb.optString("name"), alb.optString("uri"))
                                        }
                                        else -> Pair(obj.optString("name"), obj.optString("uri"))
                                    }
                                    if (name.isNotEmpty()) {
                                        names.add(name.replace("|", "/"))
                                        containerUris.add(uri)
                                    }
                                }
                            }
                        }
                        folder.startsWith("pl_tracks:") || folder == "songs" || folder == "recent" || folder == "top" -> {
                            trackUris.clear()
                            val items = json.optJSONArray("items")
                            if (items != null) {
                                for (i in 0 until items.length()) {
                                    val obj = items.getJSONObject(i)
                                    val trk = obj.optJSONObject("track") ?: obj
                                    val tName = trk.optString("name")
                                    val art = trk.optJSONArray("artists")?.optJSONObject(0)?.optString("name") ?: ""
                                    val uri = trk.optString("uri")
                                    if (tName.isNotEmpty()) {
                                        names.add((if (art.isNotEmpty()) "$tName - $art" else tName).replace("|", "/"))
                                        trackUris.add(uri)
                                    }
                                }
                            }
                        }
                        folder.startsWith("art_tracks:") -> {
                            trackUris.clear()
                            val tracks = json.optJSONArray("tracks")
                            if (tracks != null) {
                                for (i in 0 until tracks.length()) {
                                    val trk = tracks.getJSONObject(i)
                                    val tName = trk.optString("name")
                                    val uri = trk.optString("uri")
                                    if (tName.isNotEmpty()) {
                                        names.add(tName.replace("|", "/"))
                                        trackUris.add(uri)
                                    }
                                }
                            }
                        }
                        folder.startsWith("alb_tracks:") -> {
                            trackUris.clear()
                            val items = json.optJSONArray("items")
                            if (items != null) {
                                for (i in 0 until items.length()) {
                                    val trk = items.getJSONObject(i)
                                    val tName = trk.optString("name")
                                    val uri = trk.optString("uri")
                                    if (tName.isNotEmpty()) {
                                        names.add(tName.replace("|", "/"))
                                        trackUris.add(uri)
                                    }
                                }
                            }
                        }
                        folder == "queue" -> {
                            trackUris.clear()
                            val queue = json.optJSONArray("queue")
                            if (queue != null) {
                                for (i in 0 until queue.length()) {
                                    val trk = queue.getJSONObject(i)
                                    val tName = trk.optString("name")
                                    val art = trk.optJSONArray("artists")?.optJSONObject(0)?.optString("name") ?: ""
                                    val uri = trk.optString("uri")
                                    if (tName.isNotEmpty()) {
                                        names.add((if (art.isNotEmpty()) "$tName - $art" else tName).replace("|", "/"))
                                        trackUris.add(uri)
                                    }
                                }
                            }
                        }
                    }

                    if (names.isNotEmpty()) {
                        callback(names)
                        return@apiRequest
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error parsing folder $folder: ${e.message}")
                }
            }

            // Fallback
            val fallback = when {
                folder == "playlists" -> listOf("My Wave", "Cyberpunk 2077 OST", "NERV Synchro Mix", "Late Night Synth")
                folder.startsWith("pl_tracks:") -> listOf("Animal I Have Become", "Riot", "Never Too Late", "Time of Dying")
                folder == "artists" -> listOf("Three Days Grace", "Linkin Park", "Skillet", "Bring Me The Horizon")
                folder.startsWith("art_tracks:") -> listOf("In The End", "Numb", "Faint", "Crawling")
                folder == "albums" -> listOf("One-X", "Meteora", "Comatose", "Sempiternal")
                folder.startsWith("alb_tracks:") -> listOf("It's All Over", "Pain", "Animal I Have Become", "Never Too Late")
                else -> listOf("РўСЂРµРє 1", "РўСЂРµРє 2", "РўСЂРµРє 3", "РўСЂРµРє 4")
            }
            callback(fallback)
        }
    }

    fun playTrackFromFolder(context: Context, folder: String, idx: Int) {
        if (folder.startsWith("pl_tracks:")) {
            val pIdx = folder.substringAfter("pl_tracks:").toIntOrNull() ?: 0
            val pUri = containerUris.getOrNull(pIdx)
            if (pUri != null) {
                val payload = JSONObject().apply {
                    put("context_uri", pUri)
                    put("offset", JSONObject().put("position", idx))
                }.toString()
                apiRequest(context, "/me/player/play", "PUT", payload)
                return
            }
        } else if (folder.startsWith("alb_tracks:")) {
            val aIdx = folder.substringAfter("alb_tracks:").toIntOrNull() ?: 0
            val aUri = containerUris.getOrNull(aIdx)
            if (aUri != null) {
                val payload = JSONObject().apply {
                    put("context_uri", aUri)
                    put("offset", JSONObject().put("position", idx))
                }.toString()
                apiRequest(context, "/me/player/play", "PUT", payload)
                return
            }
        }

        val trackUri = trackUris.getOrNull(idx)
        if (trackUri != null) {
            val payload = JSONObject().apply {
                put("uris", org.json.JSONArray().put(trackUri))
            }.toString()
            apiRequest(context, "/me/player/play", "PUT", payload)
        }
    }

    fun playItem(context: Context, uri: String) {
        val payload = if (uri.contains(":track:")) {
            JSONObject().put("uris", org.json.JSONArray().put(uri)).toString()
        } else {
            JSONObject().put("context_uri", uri).toString()
        }
        apiRequest(context, "/me/player/play", "PUT", payload)
    }
}