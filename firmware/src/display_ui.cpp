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

void DisplayUI::setTheme(Theme theme) {
    currentTheme = theme;
    if (theme == THEME_SPOTIFY) {
        bgColor = tft.color565(18, 18, 18);
        fgColor = TFT_WHITE;
        accentColor = tft.color565(29, 185, 84); // Spotify Green
    } else {
        bgColor = TFT_WHITE;
        fgColor = TFT_BLACK;
        accentColor = TFT_BLUE;
    }
    tft.fillScreen(bgColor);
    tft.setTextColor(fgColor, bgColor);
}

void DisplayUI::setBrightness(int brightness) {
    // brightness 0-100% maps to 0-255 PWM
    int duty = (brightness * 255) / 100;
    #ifdef ESP32
    // ESP32-C3 LEDC API (Arduino core 3.x uses ledcWrite directly on pin)
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        ledcWrite(TFT_BL, duty);
    #else
        ledcWrite(0, duty);
    #endif
    #endif
}

void DisplayUI::init(bool landscape, Theme theme, int brightness) {
    isLandscape = landscape;
    tft.init();
    tft.setRotation(isLandscape ? 1 : 0); // 1 for Landscape, 0 for Portrait

    #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
    #else
    ledcAttach(TFT_BL, 5000, 8);
    #endif
    setBrightness(brightness);

    setTheme(theme);

    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tft_output);
}

void DisplayUI::showSplashScreen() {
    tft.fillScreen(bgColor);
    tft.setTextColor(fgColor, bgColor);
    tft.setCursor(isLandscape ? 50 : 15, isLandscape ? 30 : 60);
    tft.setTextSize(2);
    tft.print("iPod");
    tft.setCursor(isLandscape ? 30 : 15, isLandscape ? 50 : 80);
    tft.setTextSize(1);
    tft.print("Spotify Edition");
    tft.setCursor(isLandscape ? 60 : 25, isLandscape ? 65 : 100);
    tft.print("v" FIRMWARE_VERSION);
}

void DisplayUI::showSettingsMenu(int selectedIndex, const String& ipAddr) {
    tft.fillScreen(bgColor);
    uint16_t menuBg = (currentTheme == THEME_SPOTIFY) ? tft.color565(40,40,40) : TFT_LIGHTGREY;
    tft.fillRect(0, 0, isLandscape ? 160 : 80, 15, menuBg);
    tft.setTextColor(fgColor, menuBg);
    tft.setCursor(5, 4);
    tft.print("Settings");

    const char* items[] = {"Back", "Theme", "Rotate", "Bright", "WiFi RST", "Clr Spot", "About"};
    int maxVisible = isLandscape ? 4 : 8;
    int scrollStart = 0;
    if (selectedIndex >= maxVisible) scrollStart = selectedIndex - maxVisible + 1;

    for (int i = 0; i < maxVisible; i++) {
        int idx = scrollStart + i;
        if (idx >= 7) break;
        int y = 20 + (i * 15);
        if (idx == selectedIndex) {
            tft.fillRect(0, y - 2, isLandscape ? 160 : 80, 15, accentColor);
            tft.setTextColor((currentTheme == THEME_CLASSIC) ? TFT_WHITE : TFT_BLACK, accentColor);
        } else {
            tft.setTextColor(fgColor, bgColor);
        }
        tft.setCursor(5, y);
        tft.print(items[idx]);

        // Show IP on About
        if (idx == selectedIndex && idx == 6) {
            tft.fillRect(0, y+15, isLandscape ? 160 : 80, 15, bgColor);
            tft.setTextColor(fgColor, bgColor);
            tft.setCursor(5, y+15);
            tft.print(ipAddr);
        }
    }
}

void DisplayUI::showSetupScreen(const String& ipAddress) {
    tft.fillScreen(bgColor);
    tft.fillRect(0, 0, 80, 20, accentColor);
    tft.setTextColor(TFT_WHITE, accentColor);
    tft.setCursor(5, 5);
    tft.print("Setup");

    tft.setTextColor(fgColor, bgColor);
    tft.setCursor(5, 30);
    tft.print("Open browser:");
    tft.setCursor(5, 45);
    tft.print(ipAddress);
}

void DisplayUI::showAuthScreen() {
    tft.fillScreen(bgColor);
    tft.setCursor(5, 60);
    tft.print("Waiting for");
    tft.setCursor(5, 75);
    tft.print("Spotify login...");
}

void DisplayUI::showErrorScreen(const String& errorMsg) {
    tft.fillScreen(bgColor);
    tft.fillRect(0, 0, isLandscape ? 160 : 80, 15, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setCursor(5, 4);
    tft.print("Error");

    tft.setTextColor(fgColor, bgColor);
    tft.setCursor(5, 30);
    tft.print(errorMsg);
}

void DisplayUI::drawHeader() {
    int w = isLandscape ? 160 : 80;
    tft.fillRect(0, 0, w, 15, bgColor);
    drawPlayPauseIcon(currentIsPlaying);
    drawBattery();
}

void DisplayUI::drawPlayPauseIcon(bool isPlaying) {
    int x_offset = isLandscape ? 85 : 0;
    if (isPlaying) {
        tft.fillRect(5 + x_offset, 3, 3, 8, fgColor);
        tft.fillRect(10 + x_offset, 3, 3, 8, fgColor);
    } else {
        tft.fillTriangle(5 + x_offset, 3, 5 + x_offset, 11, 12 + x_offset, 7, fgColor);
    }
}

void DisplayUI::drawBattery() {
    int x_offset = isLandscape ? 140 : 60;
    tft.drawRect(x_offset, 4, 15, 6, fgColor);
    tft.fillRect(x_offset + 15, 6, 2, 2, fgColor);
    tft.fillRect(x_offset + 2, 5, 11, 4, fgColor); // Full battery
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
        tft.fillRect(x + 1, y + 1, fillWidth - 2, 2, accentColor);
        tft.fillRect(x + fillWidth, y + 1, barWidth - fillWidth - 2, 2, bgColor);
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
            tft.fillRect(85, 20, 75, 40, bgColor);
            if (isLiked) drawHeart(140, 45, TFT_RED);
        } else {
            tft.fillRect(0, 120, 80, 40, bgColor);
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
            tft.fillRect(85, 20, 70, 20, bgColor);
            tft.setTextColor(fgColor, bgColor);
            tft.setCursor(85, 25);
            tft.print(t);

            tft.fillRect(85, 40, 50, 15, bgColor); // Don't overwrite heart
            tft.setTextColor(TFT_DARKGREY, bgColor);
            tft.setCursor(85, 45);
            tft.print(a);
        } else {
            tft.fillRect(0, 120, 80, 15, bgColor);
            tft.setTextColor(fgColor, bgColor);
            tft.setCursor(5, 120);
            tft.print(t);

            tft.fillRect(0, 135, 60, 15, bgColor); // Don't overwrite heart
            tft.setTextColor(TFT_DARKGREY, bgColor);
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
    tft.fillRect(0, y, w, 15, bgColor);
    tft.setTextColor(fgColor, bgColor);
    tft.setCursor(5, y);
    tft.print(msg);
}
