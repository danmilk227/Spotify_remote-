package com.danmilk.ipodify

import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

object SpotifyApiHelper {
    private const val TAG = "SpotifyApiHelper"
    const val CLIENT_ID = "49be876031464f1ba5a139eb691a4ef5"
    const val REDIRECT_URI = "ipodify://callback"

    var playlistUris = mutableListOf<String>()

    fun getAccessToken(context: Context): String? {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        return prefs.getString("spotify_access_token", null)
    }

    fun saveAccessToken(context: Context, token: String) {
        val prefs = context.getSharedPreferences("ipodify_prefs", Context.MODE_PRIVATE)
        prefs.edit().putString("spotify_access_token", token).apply()
    }

    fun fetchPlaylists(context: Context, callback: (List<String>) -> Unit) {
        val token = getAccessToken(context)
        if (token == null) {
            callback(listOf("Liked Songs (Войти в Spotify)", "My Wave", "Rock Classics", "Daily Mix 1", "Workout Hits"))
            return
        }

        thread {
            try {
                val url = URL("https://api.spotify.com/v1/me/playlists?limit=25")
                val conn = url.openConnection() as HttpURLConnection
                conn.requestMethod = "GET"
                conn.setRequestProperty("Authorization", "Bearer $token")
                conn.setRequestProperty("Content-Type", "application/json")
                conn.connectTimeout = 4000
                conn.readTimeout = 4000

                if (conn.responseCode == 200) {
                    val reader = BufferedReader(InputStreamReader(conn.inputStream))
                    val body = reader.readText()
                    reader.close()

                    val json = JSONObject(body)
                    val items = json.optJSONArray("items")
                    val names = mutableListOf<String>()
                    playlistUris.clear()

                    if (items != null) {
                        for (i in 0 until items.length()) {
                            val obj = items.getJSONObject(i)
                            val name = obj.optString("name", "Playlist $i").replace("|", "/")
                            val uri = obj.optString("uri", "")
                            names.add(name)
                            playlistUris.add(uri)
                        }
                    }
                    callback(if (names.isNotEmpty()) names else listOf("Нет плейлистов"))
                } else {
                    Log.w(TAG, "Spotify API returned code: ${conn.responseCode}")
                    callback(listOf("Войдите в Spotify", "Моя волна", "Любимые треки"))
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error fetching playlists: ${e.message}")
                callback(listOf("Ошибка загрузки", "Проверьте интернет", "Моя волна"))
            }
        }
    }

    fun playItem(context: Context, uri: String) {
        val token = getAccessToken(context) ?: return
        thread {
            try {
                val url = URL("https://api.spotify.com/v1/me/player/play")
                val conn = url.openConnection() as HttpURLConnection
                conn.requestMethod = "PUT"
                conn.setRequestProperty("Authorization", "Bearer $token")
                conn.setRequestProperty("Content-Type", "application/json")
                conn.doOutput = true

                val payload = if (uri.contains(":track:")) {
                    JSONObject().put("uris", org.json.JSONArray().put(uri)).toString()
                } else {
                    JSONObject().put("context_uri", uri).toString()
                }

                val writer = OutputStreamWriter(conn.outputStream)
                writer.write(payload)
                writer.flush()
                writer.close()
                Log.i(TAG, "PlayItem response: ${conn.responseCode}")
            } catch (e: Exception) {
                Log.e(TAG, "Error starting playlist: ${e.message}")
            }
        }
    }
}
