#include "spotify_client.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <base64.h>
#include "config.h"

Preferences preferences;

SpotifyClient::SpotifyClient() : tokenExpirationTime(0) {}

bool SpotifyClient::init() {
    loadCredentials();
    return hasCredentials();
}

void SpotifyClient::loadCredentials() {
    preferences.begin("spotify", true);
    clientId = preferences.getString("clientId", "");
    clientSecret = preferences.getString("clientSecret", "");
    refreshToken = preferences.getString("refreshToken", "");
    preferences.end();
}

void SpotifyClient::saveCredentials() {
    preferences.begin("spotify", false);
    preferences.putString("clientId", clientId);
    preferences.putString("clientSecret", clientSecret);
    preferences.putString("refreshToken", refreshToken);
    preferences.end();
}

void SpotifyClient::setCredentials(const String& id, const String& secret, const String& token) {
    clientId = id;
    clientSecret = secret;
    refreshToken = token;
    saveCredentials();
}

bool SpotifyClient::hasCredentials() {
    return clientId.length() > 0 && clientSecret.length() > 0 && refreshToken.length() > 0;
}

bool SpotifyClient::refreshAccessToken() {
    if (millis() < tokenExpirationTime && accessToken.length() > 0) {
        return true;
    }

    WiFiClientSecure client;
    client.setCACert(spotify_ca_cert);
    HTTPClient http;

    http.begin(client, SPOTIFY_TOKEN_URL);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String auth = clientId + ":" + clientSecret;
    String authBase64 = base64::encode(auth);
    http.addHeader("Authorization", "Basic " + authBase64);

    String payload = "grant_type=refresh_token&refresh_token=" + refreshToken;

    int httpCode = http.POST(payload);
    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, response);

        accessToken = doc["access_token"].as<String>();
        int expiresIn = doc["expires_in"].as<int>();
        tokenExpirationTime = millis() + (expiresIn - 60) * 1000; // Refresh 60s early
        success = true;
    } else {
        Serial.printf("Failed to refresh token, HTTP Code: %d\n", httpCode);
    }

    http.end();
    return success;
}

String SpotifyClient::makeApiRequest(const char* endpoint, const char* method, const char* payload, int* outHttpCode) {
    if (!refreshAccessToken()) {
        if (outHttpCode) *outHttpCode = -1;
        return "";
    }

    WiFiClientSecure client;
    client.setCACert(spotify_ca_cert);
    HTTPClient http;

    String url = String(SPOTIFY_API_BASE) + endpoint;
    http.begin(client, url);
    http.addHeader("Authorization", "Bearer " + accessToken);

    int httpCode;
    if (String(method) == "GET") {
        httpCode = http.GET();
    } else if (String(method) == "POST") {
        httpCode = http.POST(payload ? payload : "");
    } else if (String(method) == "PUT") {
        http.addHeader("Content-Length", "0");
        httpCode = http.PUT(payload ? payload : "");
    } else if (String(method) == "DELETE") {
        httpCode = http.sendRequest("DELETE", payload ? payload : "");
    } else {
        httpCode = http.sendRequest(method, payload ? payload : "");
    }

    if (outHttpCode) *outHttpCode = httpCode;

    String response = "";
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT) {
        response = http.getString();
    }

    http.end();
    return response;
}

bool SpotifyClient::updateTrackInfo() {
    int httpCode = 0;
    String response = makeApiRequest("/me/player/currently-playing", "GET", nullptr, &httpCode);

    if (httpCode == 204) {
        // No active device playing
        currentTrack.title = "No active device";
        currentTrack.artist = "";
        currentTrack.isPlaying = false;
        currentTrack.albumArtUrl = "";
        return true;
    } else if (httpCode == 401) {
        // Force token refresh on next tick
        // TODO: In a production device, we should consider PKCE (Proof Key for Code Exchange)
        // so we don't have to store the Client Secret in NVS in plain text.
        tokenExpirationTime = 0;
        return false;
    } else if (httpCode == 429) {
        currentTrack.title = "Rate limited";
        return false;
    } else if (httpCode < 0) {
        currentTrack.title = "Network error";
        return false;
    }

    if (response.length() == 0) return false;

    // Use a filter to skip massive arrays (like available_markets) to save memory
    StaticJsonDocument<200> filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["item"]["duration_ms"] = true;
    filter["item"]["name"] = true;
    filter["item"]["artists"][0]["name"] = true;
    filter["item"]["id"] = true;
    filter["item"]["album"]["images"][0]["url"] = true;

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, response, DeserializationOption::Filter(filter));
    if (error) return false;

    if (!doc["item"].isNull()) {
        currentTrack.isPlaying = doc["is_playing"].as<bool>();
        currentTrack.progressMs = doc["progress_ms"].as<int>();
        currentTrack.durationMs = doc["item"]["duration_ms"].as<int>();
        currentTrack.title = doc["item"]["name"].as<String>();
        currentTrack.artist = doc["item"]["artists"][0]["name"].as<String>();
        currentTrack.trackId = doc["item"]["id"].as<String>();

        JsonArray images = doc["item"]["album"]["images"];
        if (images.size() > 0) {
            // Get the smallest image (usually last, 64x64) for faster loading, or middle one (300x300)
            int imgIndex = images.size() - 1;
            currentTrack.albumArtUrl = images[imgIndex]["url"].as<String>();
        }

        // Check if liked
        int likeHttpCode = 0;
        String likeResponse = makeApiRequest(("/me/tracks/contains?ids=" + currentTrack.trackId).c_str(), "GET", nullptr, &likeHttpCode);
        if (likeResponse.length() > 0 && likeHttpCode == HTTP_CODE_OK) {
            DynamicJsonDocument likeDoc(256);
            deserializeJson(likeDoc, likeResponse);
            currentTrack.isLiked = likeDoc[0].as<bool>();
        }

        return true;
    }

    currentTrack.isPlaying = false;
    return true; // Parsed successfully but nothing playing
}

void SpotifyClient::playPause() {
    if (currentTrack.isPlaying) {
        makeApiRequest("/me/player/pause", "PUT");
    } else {
        makeApiRequest("/me/player/play", "PUT");
    }
}

void SpotifyClient::nextTrack() {
    makeApiRequest("/me/player/next", "POST");
}

void SpotifyClient::prevTrack() {
    makeApiRequest("/me/player/previous", "POST");
}

void SpotifyClient::toggleLike() {
    if (currentTrack.trackId.length() == 0) return;
    String endpoint = "/me/tracks?ids=" + currentTrack.trackId;
    if (currentTrack.isLiked) {
        makeApiRequest(endpoint.c_str(), "DELETE");
    } else {
        makeApiRequest(endpoint.c_str(), "PUT");
    }
}

bool SpotifyClient::fetchAlbumArt(uint8_t** buffer, size_t* size) {
    if (currentTrack.albumArtUrl.length() == 0) return false;

    WiFiClientSecure client;
    client.setCACert(spotify_ca_cert);
    HTTPClient http;

    http.begin(client, currentTrack.albumArtUrl);
    int httpCode = http.GET();

    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        int len = http.getSize();
        if (len > 0) {
            *size = len;
            *buffer = (uint8_t*)malloc(*size);
            if (*buffer) {
                WiFiClient* stream = http.getStreamPtr();
                size_t bytesRead = 0;
                while(http.connected() && (bytesRead < *size)) {
                    size_t available = stream->available();
                    if (available) {
                        int c = stream->readBytes((*buffer) + bytesRead, available);
                        bytesRead += c;
                    }
                    delay(1);
                }
                success = (bytesRead == *size);
                if (!success) {
                    free(*buffer);
                    *buffer = nullptr;
                }
            }
        }
    }
    http.end();
    return success;
}
