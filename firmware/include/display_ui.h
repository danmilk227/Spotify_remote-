#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

class DisplayUI {
public:
    DisplayUI();
    void init(bool landscape = false);
    void showSplashScreen();
    void showSetupScreen(const String& ipAddress);
    void showAuthScreen();
    void updatePlayer(const String& title, const String& artist, int progressMs, int durationMs, bool isPlaying, bool isLiked);
    void drawAlbumCover(const uint8_t* jpegData, size_t jpegSize);
    void showMessage(const String& msg);

private:
    TFT_eSPI tft;
    static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);
    void drawBattery();
    void drawPlayPauseIcon(bool isPlaying);
    void drawProgressBar(int progressMs, int durationMs);
    void drawHeader();

    String currentTitle;
    String currentArtist;
    int currentProgress;
    int currentDuration;
    bool currentIsPlaying;
    bool currentIsLiked;
    bool isLandscape;
};
