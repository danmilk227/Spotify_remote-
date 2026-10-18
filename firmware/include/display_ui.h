#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

enum Theme {
    THEME_CLASSIC,
    THEME_SPOTIFY
};

class DisplayUI {
public:
    DisplayUI();
    void init(bool landscape = false, Theme theme = THEME_CLASSIC, int brightness = 100);
    void setTheme(Theme theme);
    void setBrightness(int brightness); // 0-100%
    void showSplashScreen();
    void showSetupScreen(const String& ipAddress);
    void showAuthScreen();
    void showSettingsMenu(int selectedIndex, const String& ipAddr);
    void updatePlayer(const String& title, const String& artist, int progressMs, int durationMs, bool isPlaying, bool isLiked);
    void drawAlbumCover(const uint8_t* jpegData, size_t jpegSize);
    void showMessage(const String& msg);
    void showErrorScreen(const String& errorMsg);
    void tickMarquee();

private:
    TFT_eSPI tft;
    static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);
    void drawBattery();
    void drawPlayPauseIcon(bool isPlaying);
    void drawProgressBar(int progressMs, int durationMs);
    void drawHeader();
    void drawHeart(int x, int y, uint16_t color);

    Theme currentTheme;
    uint16_t bgColor;
    uint16_t fgColor;
    uint16_t accentColor;

    String currentTitle;
    String currentArtist;

    // Marquee variables
    int titleScrollOffset;
    int artistScrollOffset;
    unsigned long lastMarqueeUpdate;

    int currentProgress;
    int currentDuration;
    bool currentIsPlaying;
    bool currentIsLiked;
    bool isLandscape;
};
