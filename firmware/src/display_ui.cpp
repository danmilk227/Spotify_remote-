#include "display_ui.h"
#include <TJpg_Decoder.h>

DisplayUI* instance = nullptr;

DisplayUI::DisplayUI() : currentProgress(0), currentDuration(1), currentIsPlaying(false), currentIsLiked(false), isLandscape(false) {
    instance = this;
}

bool DisplayUI::tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= instance->tft.height()) return 0;
    instance->tft.pushImage(x, y, w, h, bitmap);
    return 1;
}

void DisplayUI::init(bool landscape) {
    isLandscape = landscape;
    tft.init();
    tft.setRotation(isLandscape ? 1 : 0); // 1 for Landscape, 0 for Portrait
    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);

    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tft_output);
}

void DisplayUI::showSplashScreen() {
    tft.fillScreen(TFT_WHITE);
    tft.setCursor(15, 60);
    tft.setTextSize(2);
    tft.print("iPod");
    tft.setCursor(15, 80);
    tft.setTextSize(1);
    tft.print("Spotify Edition");
}

void DisplayUI::showSetupScreen(const String& ipAddress) {
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, 80, 20, TFT_LIGHTGREY);
    tft.setTextColor(TFT_BLACK, TFT_LIGHTGREY);
    tft.setCursor(5, 5);
    tft.print("Setup");

    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setCursor(5, 30);
    tft.print("Open browser:");
    tft.setCursor(5, 45);
    tft.print(ipAddress);
}

void DisplayUI::showAuthScreen() {
    tft.fillScreen(TFT_WHITE);
    tft.setCursor(5, 60);
    tft.print("Waiting for");
    tft.setCursor(5, 75);
    tft.print("Spotify login...");
}

void DisplayUI::drawHeader() {
    int w = isLandscape ? 160 : 80;
    tft.fillRect(0, 0, w, 15, TFT_WHITE);
    drawPlayPauseIcon(currentIsPlaying);
    drawBattery();
}

void DisplayUI::drawPlayPauseIcon(bool isPlaying) {
    int x_offset = isLandscape ? 85 : 0;
    if (isPlaying) {
        tft.fillRect(5 + x_offset, 3, 3, 8, TFT_BLACK);
        tft.fillRect(10 + x_offset, 3, 3, 8, TFT_BLACK);
    } else {
        tft.fillTriangle(5 + x_offset, 3, 5 + x_offset, 11, 12 + x_offset, 7, TFT_BLACK);
    }
}

void DisplayUI::drawBattery() {
    int x_offset = isLandscape ? 140 : 60;
    tft.drawRect(x_offset, 4, 15, 6, TFT_BLACK);
    tft.fillRect(x_offset + 15, 6, 2, 2, TFT_BLACK);
    tft.fillRect(x_offset + 2, 5, 11, 4, TFT_BLACK); // Full battery
}

void DisplayUI::drawProgressBar(int progressMs, int durationMs) {
    int barWidth = isLandscape ? 70 : 70;
    int x = isLandscape ? 85 : 5;
    int y = isLandscape ? 65 : 110;

    tft.drawRect(x, y, barWidth, 4, TFT_DARKGREY);

    if (durationMs > 0) {
        int fillWidth = (progressMs * barWidth) / durationMs;
        tft.fillRect(x + 1, y + 1, fillWidth - 2, 2, TFT_BLUE);
        tft.fillRect(x + fillWidth, y + 1, barWidth - fillWidth - 2, 2, TFT_WHITE);
    }
}

void DisplayUI::updatePlayer(const String& title, const String& artist, int progressMs, int durationMs, bool isPlaying, bool isLiked) {
    if (title != currentTitle || artist != currentArtist || isLiked != currentIsLiked) {
        if (isLandscape) {
            tft.fillRect(85, 20, 75, 40, TFT_WHITE);
            tft.setTextColor(TFT_BLACK, TFT_WHITE);
            tft.setTextWrap(false);
            tft.setCursor(85, 25);
            tft.print(title.substring(0, 11)); // Truncate to fit

            tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
            tft.setCursor(85, 45);
            tft.print(artist.substring(0, 11));

            if (isLiked) {
                tft.setTextColor(TFT_RED, TFT_WHITE);
                tft.setCursor(140, 45);
                tft.print("<3"); // Heart
            }
        } else {
            tft.fillRect(0, 120, 80, 40, TFT_WHITE);
            tft.setTextColor(TFT_BLACK, TFT_WHITE);
            tft.setTextWrap(false);
            tft.setCursor(5, 120);
            tft.print(title.substring(0, 12)); // Truncate to fit

            tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
            tft.setCursor(5, 135);
            tft.print(artist.substring(0, 12));

            if (isLiked) {
                tft.setTextColor(TFT_RED, TFT_WHITE);
                tft.setCursor(65, 135);
                tft.print("<3"); // Heart
            }
        }

        currentTitle = title;
        currentArtist = artist;
        currentIsLiked = isLiked;
    }

    if (isPlaying != currentIsPlaying) {
        drawHeader();
        currentIsPlaying = isPlaying;
    }

    drawProgressBar(progressMs, durationMs);
}

void DisplayUI::drawAlbumCover(const uint8_t* jpegData, size_t jpegSize) {
    if (jpegData && jpegSize > 0) {
        if (isLandscape) {
            // Draw image on the left side, slightly lower to avoid header overlap
            TJpgDec.drawJpg(0, 15, jpegData, jpegSize);
        } else {
            // Draw image centered in the 80x80 area below header
            TJpgDec.drawJpg(8, 20, jpegData, jpegSize);
        }
    }
}

void DisplayUI::showMessage(const String& msg) {
    int y = isLandscape ? 65 : 145;
    int w = isLandscape ? 160 : 80;
    tft.fillRect(0, y, w, 15, TFT_WHITE);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setCursor(5, y);
    tft.print(msg);
}
