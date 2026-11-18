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
import java.security.MessageDigest
import java.security.SecureRandom
import kotlin.concurrent.thread

object SpotifyApiHelper {
    private const val TAG = "SpotifyApiHelper"
    const val DEFAULT_CLIENT_ID = "49be876031464f1ba5a139eb691a4ef5"
    const val REDIRECT_URI = "ipodify://callback"

    var playlistUris = mutableListOf<String>()

    data class PlaybackStateInfo(
        val isPlaying: Boolean,
        val title: String,
        val artist: String,
        val album: String,
        val posSec: Int,
        val durSec: Int,
        val volumePct: Int,
        val isShuffled: Boolean,
        val repeatState: Int // 0: off, 1: context, 2: track
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

        return "https://accounts.spotify.com/authorize?client_id=$clientId&response_type=code&redirect_uri=$REDIRECT_URI&code_challenge_method=S256&code_challenge=$challenge&scope=$scope"
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

                val postParams = "client_id=$clientId&grant_type=authorization_code&code=$code&redirect_uri=$REDIRECT_URI&code_verifier=$verifier"
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

                val postParams = "client_id=$clientId&grant_type=refresh_token&refresh_token=$refreshToken"
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

                        val info = PlaybackStateInfo(
                            isPlaying = isPlaying,
                            title = title,
                            artist = artist,
                            album = album,
                            posSec = (progressMs / 1000).toInt(),
                            durSec = (durMs / 1000).toInt().coerceAtLeast(1),
                            volumePct = volPct,
                            isShuffled = shuffleState,
                            repeatState = repMode
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
        val endpoint = when (folder) {
            "playlists" -> "/me/playlists?limit=25"
            "artists" -> "/me/following?type=artist&limit=25"
            "albums" -> "/me/albums?limit=25"
            "recent" -> "/me/player/recently-played?limit=25"
            else -> "/me/tracks?limit=25"
        }

        apiRequest(context, endpoint, "GET") { code, body ->
            if (code == 200 && !body.isNullOrEmpty()) {
                try {
                    val json = JSONObject(body)
                    val names = mutableListOf<String>()
                    playlistUris.clear()

                    val items = if (folder == "artists") {
                        json.optJSONObject("artists")?.optJSONArray("items")
                    } else {
                        json.optJSONArray("items")
                    }

                    if (items != null) {
                        for (i in 0 until items.length()) {
                            val obj = items.getJSONObject(i)
                            val (name, uri) = when (folder) {
                                "playlists" -> Pair(obj.optString("name"), obj.optString("uri"))
                                "artists" -> Pair(obj.optString("name"), obj.optString("uri"))
                                "albums" -> {
                                    val alb = obj.optJSONObject("album") ?: obj
                                    Pair(alb.optString("name"), alb.optString("uri"))
                                }
                                "recent", "songs" -> {
                                    val trk = obj.optJSONObject("track") ?: obj
                                    val art = trk.optJSONArray("artists")?.optJSONObject(0)?.optString("name") ?: ""
                                    val tName = trk.optString("name")
                                    Pair(if (art.isNotEmpty()) "$tName - $art" else tName, trk.optString("uri"))
                                }
                                else -> Pair(obj.optString("name"), obj.optString("uri"))
                            }

                            if (name.isNotEmpty()) {
                                names.add(name.replace("|", "/"))
                                playlistUris.add(uri)
                            }
                        }
                    }

                    callback(if (names.isNotEmpty()) names else listOf("Пусто"))
                    return@apiRequest
                } catch (e: Exception) {
                    Log.e(TAG, "Error parsing folder $folder: ${e.message}")
                }
            }
            // Fallback
            val fallback = when (folder) {
                "playlists" -> listOf("Войдите в Spotify", "Моя волна", "Любимые треки", "Рок Хиты")
                "artists" -> listOf("Three Days Grace", "Linkin Park", "Skillet", "Bring Me The Horizon")
                "albums" -> listOf("One-X", "Meteora", "Comatose", "Sempiternal")
                else -> listOf("Трек 1", "Трек 2", "Трек 3", "Трек 4")
            }
            callback(fallback)
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
