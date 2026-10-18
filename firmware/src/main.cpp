#include <Arduino.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "display_ui.h"
#include "spotify_client.h"

#define WDT_TIMEOUT 15 // 15 seconds WDT

DisplayUI ui;
SpotifyClient spotify;
WebServer server(80);

unsigned long lastUpdate = 0;
unsigned long currentUpdateInterval = 2500; // Fast polling initial

// Button debouncing variables
unsigned long lastBtnPress[5] = {0, 0, 0, 0, 0};
int lastBtnState[5] = {HIGH, HIGH, HIGH, HIGH, HIGH}; // HIGH = unpressed
const int DEBOUNCE_DELAY = 50;

// Menu State
bool inSettingsMenu = false;
int menuSelection = 0;
unsigned long menuBtnPressStart = 0;
bool menuBtnLongPressed = false;

// Hardware State
int currentBrightness = 100;
Theme currentTheme = THEME_CLASSIC;

// Vol Control
unsigned long leftBtnPressStart = 0;
unsigned long rightBtnPressStart = 0;

String lastTrackId = "";

void handleRoot() {
    String html = "<html><body><h2>Spotify Setup</h2>"
                  "<form action='/save' method='POST'>"
                  "Client ID:<br><input type='text' name='clientId'><br>"
                  "Client Secret:<br><input type='text' name='clientSecret'><br>"
                  "Refresh Token:<br><input type='text' name='refreshToken'><br><br>"
                  "Screen Orientation:<br>"
                  "<select name='orientation'>"
                  "<option value='portrait'>Portrait (Vertical)</option>"
                  "<option value='landscape'>Landscape (Horizontal)</option>"
                  "</select><br><br>"
                  "<input type='submit' value='Save'>"
                  "</form></body></html>";
    server.send(200, "text/html", html);
}

void handleSave() {
    if (server.hasArg("clientId") && server.hasArg("clientSecret") && server.hasArg("refreshToken")) {
        spotify.setCredentials(server.arg("clientId"), server.arg("clientSecret"), server.arg("refreshToken"));

        bool isLandscape = (server.arg("orientation") == "landscape");
        Preferences prefs;
        prefs.begin("settings", false);
        prefs.putBool("landscape", isLandscape);
        prefs.end();

        server.send(200, "text/plain", "Saved! Rebooting...");
        delay(1000);
        ESP.restart();
    } else {
        server.send(400, "text/plain", "Missing arguments");
    }
}

bool checkButton(int pin, int index) {
    int currentState = digitalRead(pin);
    if (currentState != lastBtnState[index]) {
        lastBtnPress[index] = millis();
    }

    if ((millis() - lastBtnPress[index]) > DEBOUNCE_DELAY) {
        if (currentState == LOW && lastBtnState[index] == HIGH) { // Button just pressed
            lastBtnState[index] = currentState;
            return true;
        }
        lastBtnState[index] = currentState;
    }
    return false;
}

void setup() {
    Serial.begin(115200);

    // Setup Buttons
    pinMode(BTN_UP_PIN, INPUT_PULLUP);
    pinMode(BTN_DOWN_PIN, INPUT_PULLUP);
    pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
    pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
    pinMode(BTN_CENTER_PIN, INPUT_PULLUP);

    Preferences prefs;
    prefs.begin("settings", true);
    bool isLandscape = prefs.getBool("landscape", false);
    currentBrightness = prefs.getInt("brightness", 100);
    currentTheme = (Theme)prefs.getInt("theme", THEME_CLASSIC);
    prefs.end();

    ui.init(isLandscape, currentTheme, currentBrightness);
    ui.showSplashScreen();
    delay(2000);

    WiFiManager wm;
    bool res;
    // Captive portal for WiFi setup
    res = wm.autoConnect("iPod-Nano-Setup");

    if(!res) {
        Serial.println("Failed to connect");
        ESP.restart();
    }

    esp_task_wdt_init(WDT_TIMEOUT, true);
    esp_task_wdt_add(NULL);

    if (!spotify.init()) {
        ui.showSetupScreen(WiFi.localIP().toString());
        server.on("/", handleRoot);
        server.on("/save", handleSave);
        server.begin();

        while(!spotify.hasCredentials()) {
            esp_task_wdt_reset(); // Keep WDT happy during setup
            server.handleClient();
            delay(10);
        }
    }

    ui.showMessage("Connecting Spotify...");
}

void handleMenuLogic() {
    if (checkButton(BTN_DOWN_PIN, 1)) {
        menuSelection = (menuSelection + 1) % 7;
        ui.showSettingsMenu(menuSelection, WiFi.localIP().toString());
    }
    if (checkButton(BTN_UP_PIN, 0)) {
        menuSelection = (menuSelection - 1 + 7) % 7;
        ui.showSettingsMenu(menuSelection, WiFi.localIP().toString());
    }
    if (checkButton(BTN_CENTER_PIN, 4)) {
        Preferences prefs;
        prefs.begin("settings", false);

        if (menuSelection == 0) { // Back
            inSettingsMenu = false;
            lastTrackId = ""; // force redraw
        } else if (menuSelection == 1) { // Theme
            currentTheme = (currentTheme == THEME_CLASSIC) ? THEME_SPOTIFY : THEME_CLASSIC;
            prefs.putInt("theme", currentTheme);
            ui.setTheme(currentTheme);
            ui.showSettingsMenu(menuSelection, WiFi.localIP().toString());
        } else if (menuSelection == 2) { // Rotate
            bool isLand = !prefs.getBool("landscape", false);
            prefs.putBool("landscape", isLand);
            ui.showMessage("Rebooting...");
            delay(1000);
            ESP.restart();
        } else if (menuSelection == 3) { // Brightness
            currentBrightness -= 25;
            if (currentBrightness <= 0) currentBrightness = 100;
            prefs.putInt("brightness", currentBrightness);
            ui.setBrightness(currentBrightness);
            ui.showSettingsMenu(menuSelection, WiFi.localIP().toString());
        } else if (menuSelection == 4) { // WiFi RST
            WiFiManager wm;
            wm.resetSettings();
            ui.showMessage("WiFi Reset! Rebooting...");
            delay(1000);
            ESP.restart();
        } else if (menuSelection == 5) { // Clear Spotify
            spotify.clearCredentials();
            ui.showMessage("Creds Cleared! Rebooting...");
            delay(1000);
            ESP.restart();
        } else if (menuSelection == 6) { // About
            // Do nothing, IP is drawn
        }
        prefs.end();
    }
}

void loop() {
    esp_task_wdt_reset();

    // Auto-reconnect WiFi if dropped (rate-limited)
    static unsigned long lastWiFiCheck = 0;
    if (WiFi.status() != WL_CONNECTED && (millis() - lastWiFiCheck > 10000)) {
        lastWiFiCheck = millis();
        WiFi.reconnect();
    }

    // Handle Long Press on Menu Button (GPIO 0)
    int menuState = digitalRead(BTN_UP_PIN);
    if (menuState == LOW && lastBtnState[0] == HIGH) {
        menuBtnPressStart = millis();
        menuBtnLongPressed = false;
    } else if (menuState == LOW && !menuBtnLongPressed && (millis() - menuBtnPressStart > 1500)) {
        menuBtnLongPressed = true;
        inSettingsMenu = !inSettingsMenu;
        if (inSettingsMenu) {
            menuSelection = 0;
            ui.showSettingsMenu(menuSelection, WiFi.localIP().toString());
        } else {
            lastTrackId = ""; // force redraw of player
            lastUpdate = 0; // force immediate fetch
        }
    }
    // Update button state array for Menu button manually for long press
    if (menuState != lastBtnState[0] && (millis() - lastBtnPress[0] > DEBOUNCE_DELAY)) {
         lastBtnState[0] = menuState;
         lastBtnPress[0] = millis();
    }


    if (inSettingsMenu) {
        handleMenuLogic();
        return; // Don't do spotify stuff while in menu
    }

    ui.tickMarquee();

    if (millis() - lastUpdate > currentUpdateInterval) {
        if (spotify.updateTrackInfo()) {
            const TrackInfo& track = spotify.getTrackInfo();

            if (track.errorMessage.length() > 0) {
                ui.showErrorScreen(track.errorMessage);
                lastTrackId = "error"; // Force redraw when recovers
            } else {
                ui.updatePlayer(track.title, track.artist, track.progressMs, track.durationMs, track.isPlaying, track.isLiked);

            // Adaptive polling
            currentUpdateInterval = track.isPlaying ? 2500 : 8000;

            if (track.trackId != lastTrackId) {
                lastTrackId = track.trackId;
                uint8_t* imgBuffer = nullptr;
                size_t imgSize = 0;
                if (spotify.fetchAlbumArt(&imgBuffer, &imgSize)) {
                    ui.drawAlbumCover(imgBuffer, imgSize);
                    free(imgBuffer);
                }
            }
        }
        lastUpdate = millis(); // Update after network requests
    }

    // Handle Buttons (Player Controls)
    if (checkButton(BTN_DOWN_PIN, 1)) {
        spotify.playPause();
        lastUpdate = 0; // Immediate refresh
    }

    // Right Button (Next / Vol Up)
    int rightState = digitalRead(BTN_RIGHT_PIN);
    if (rightState != lastBtnState[3]) {
        if (rightState == LOW) rightBtnPressStart = millis(); // Pressed
        else if (millis() - rightBtnPressStart < 600 && lastBtnState[3] == LOW) {
            // Short press released
            spotify.nextTrack();
            lastTrackId = "";
            lastUpdate = 0;
        }
        lastBtnState[3] = rightState;
    } else if (rightState == LOW && (millis() - rightBtnPressStart > 600)) {
        // Held down
        rightBtnPressStart = millis(); // reset to fire again
        spotify.setVolume(spotify.getTrackInfo().volumePercent + 5);
        ui.showMessage("Vol " + String(spotify.getTrackInfo().volumePercent));
    }

    // Left Button (Prev / Vol Down)
    int leftState = digitalRead(BTN_LEFT_PIN);
    if (leftState != lastBtnState[2]) {
        if (leftState == LOW) leftBtnPressStart = millis(); // Pressed
        else if (millis() - leftBtnPressStart < 600 && lastBtnState[2] == LOW) {
            // Short press released
            spotify.prevTrack();
            lastTrackId = "";
            lastUpdate = 0;
        }
        lastBtnState[2] = leftState;
    } else if (leftState == LOW && (millis() - leftBtnPressStart > 600)) {
        // Held down
        leftBtnPressStart = millis(); // reset to fire again
        spotify.setVolume(spotify.getTrackInfo().volumePercent - 5);
        ui.showMessage("Vol " + String(spotify.getTrackInfo().volumePercent));
    }

    if (checkButton(BTN_CENTER_PIN, 4)) {
        spotify.toggleLike();
        lastUpdate = 0;
    }
}
}
