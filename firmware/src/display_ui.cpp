#include "display_ui.h"
#include <TJpg_Decoder.h>

DisplayUI* instance = nullptr;

#include "config.h"

DisplayUI::DisplayUI() : currentTitle(""), currentArtist(""), titleScrollOffset(0), artistScrollOffset(0), lastMarqueeUpdate(0),
                         currentProgress(0), currentDuration(1), currentIsPlaying(false), currentIsLiked(false), isLandscape(false) {
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
    tft.setCursor(isLandscape ? 50 : 15, isLandscape ? 30 : 60);
    tft.setTextSize(2);
    tft.print("iPod");
    tft.setCursor(isLandscape ? 30 : 15, isLandscape ? 50 : 80);
    tft.setTextSize(1);
    tft.print("Spotify Edition");
    tft.setCursor(isLandscape ? 60 : 25, isLandscape ? 65 : 100);
    tft.print("v" FIRMWARE_VERSION);
}

void DisplayUI::showSettingsMenu(int selectedIndex) {
    tft.fillScreen(TFT_WHITE);
    tft.fillRect(0, 0, isLandscape ? 160 : 80, 15, TFT_LIGHTGREY);
    tft.setTextColor(TFT_BLACK, TFT_LIGHTGREY);
    tft.setCursor(5, 4);
    tft.print("Settings");

    const char* items[] = {"Back", "Rotate", "WiFi Reset"};
    for (int i = 0; i < 3; i++) {
        int y = 25 + (i * 15);
        if (i == selectedIndex) {
            tft.fillRect(0, y - 2, isLandscape ? 160 : 80, 15, TFT_BLUE);
            tft.setTextColor(TFT_WHITE, TFT_BLUE);
        } else {
            tft.setTextColor(TFT_BLACK, TFT_WHITE);
        }
        tft.setCursor(5, y);
        tft.print(items[i]);
    }
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

void DisplayUI::drawHeart(int x, int y, uint16_t color) {
    tft.fillTriangle(x, y+2, x+3, y-1, x+6, y+2, color);
    tft.fillTriangle(x+6, y+2, x+9, y-1, x+12, y+2, color);
    tft.fillTriangle(x, y+2, x+12, y+2, x+6, y+8, color);
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
        currentTitle = title;
        currentArtist = artist;
        currentIsLiked = isLiked;
        titleScrollOffset = 0;
        artistScrollOffset = 0;
        lastMarqueeUpdate = millis();

        if (isLandscape) {
            tft.fillRect(85, 20, 75, 40, TFT_WHITE);
            if (isLiked) drawHeart(140, 45, TFT_RED);
        } else {
            tft.fillRect(0, 120, 80, 40, TFT_WHITE);
            if (isLiked) drawHeart(65, 135, TFT_RED);
        }

        // Force an immediate marquee draw
        tickMarquee();
    }

    if (isPlaying != currentIsPlaying) {
        drawHeader();
        currentIsPlaying = isPlaying;
    }

    drawProgressBar(progressMs, durationMs);
}

void DisplayUI::tickMarquee() {
    if (currentTitle.length() == 0 && currentArtist.length() == 0) return;

    if (millis() - lastMarqueeUpdate > 250) {
        lastMarqueeUpdate = millis();

        int maxChars = isLandscape ? 11 : 12;
        String t = currentTitle;
        String a = currentArtist;

        // If string is longer than screen, scroll it
        if (t.length() > maxChars) {
            t = t + "   " + t; // Add gap for looping
            t = t.substring(titleScrollOffset, titleScrollOffset + maxChars);
            titleScrollOffset++;
            if (titleScrollOffset > currentTitle.length() + 3) titleScrollOffset = 0;
        }

        if (a.length() > maxChars) {
            a = a + "   " + a;
            a = a.substring(artistScrollOffset, artistScrollOffset + maxChars);
            artistScrollOffset++;
            if (artistScrollOffset > currentArtist.length() + 3) artistScrollOffset = 0;
        }

        if (isLandscape) {
            tft.fillRect(85, 20, 70, 20, TFT_WHITE);
            tft.setTextColor(TFT_BLACK, TFT_WHITE);
            tft.setCursor(85, 25);
            tft.print(t);

            tft.fillRect(85, 40, 50, 15, TFT_WHITE); // Don't overwrite heart
            tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
            tft.setCursor(85, 45);
            tft.print(a);
        } else {
            tft.fillRect(0, 120, 80, 15, TFT_WHITE);
            tft.setTextColor(TFT_BLACK, TFT_WHITE);
            tft.setCursor(5, 120);
            tft.print(t);

            tft.fillRect(0, 135, 60, 15, TFT_WHITE); // Don't overwrite heart
            tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
            tft.setCursor(5, 135);
            tft.print(a);
        }
    }
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
