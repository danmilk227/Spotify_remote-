#pragma once

#include <Arduino.h>
#include <HTTPClient.h>

struct TrackInfo {
    String title;
    String artist;
    String albumArtUrl;
    int progressMs;
    int durationMs;
    bool isPlaying;
    bool isLiked;
    String trackId;
    int volumePercent;
    String errorMessage;
};

class SpotifyClient {
public:
    SpotifyClient();
    bool init();
    void setCredentials(const String& clientId, const String& clientSecret, const String& refreshToken);
    bool hasCredentials();
    void clearCredentials();

    bool updateTrackInfo();
    const TrackInfo& getTrackInfo() const { return currentTrack; }

    // Controls
    void playPause();
    void nextTrack();
    void prevTrack();
    void toggleLike();
    void setVolume(int volume_percent);

    // Album Art
    bool fetchAlbumArt(uint8_t** buffer, size_t* size);

private:
    String clientId;
    String clientSecret;
    String refreshToken;
    String accessToken;
    unsigned long tokenExpirationTime;

    TrackInfo currentTrack;

    bool refreshAccessToken();
    String makeApiRequest(const char* endpoint, const char* method = "GET", const char* payload = nullptr, int* outHttpCode = nullptr);
    void loadCredentials();
    void saveCredentials();
};
