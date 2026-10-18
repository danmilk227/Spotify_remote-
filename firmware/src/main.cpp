#include <Arduino.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include "config.h"
#include "display_ui.h"
#include "spotify_client.h"

DisplayUI ui;
SpotifyClient spotify;
WebServer server(80);

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 5000; // 5 seconds

// Button debouncing variables
unsigned long lastBtnPress[5] = {0, 0, 0, 0, 0};
const int DEBOUNCE_DELAY = 250;

String lastTrackId = "";

void handleRoot() {
    String html = "<html><body><h2>Spotify Setup</h2>"
                  "<form action='/save' method='POST'>"
                  "Client ID:<br><input type='text' name='clientId'><br>"
                  "Client Secret:<br><input type='text' name='clientSecret'><br>"
                  "Refresh Token:<br><input type='text' name='refreshToken'><br><br>"
                  "<input type='submit' value='Save'>"
                  "</form></body></html>";
    server.send(200, "text/html", html);
}

void handleSave() {
    if (server.hasArg("clientId") && server.hasArg("clientSecret") && server.hasArg("refreshToken")) {
        spotify.setCredentials(server.arg("clientId"), server.arg("clientSecret"), server.arg("refreshToken"));
        server.send(200, "text/plain", "Saved! Rebooting...");
        delay(1000);
        ESP.restart();
    } else {
        server.send(400, "text/plain", "Missing arguments");
    }
}

bool checkButton(int pin, int index) {
    if (digitalRead(pin) == LOW) { // Assuming buttons are pulled up
        if (millis() - lastBtnPress[index] > DEBOUNCE_DELAY) {
            lastBtnPress[index] = millis();
            return true;
        }
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

    ui.init();
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

    if (!spotify.init()) {
        ui.showSetupScreen(WiFi.localIP().toString());
        server.on("/", handleRoot);
        server.on("/save", handleSave);
        server.begin();

        while(!spotify.hasCredentials()) {
            server.handleClient();
            delay(10);
        }
    }

    ui.showMessage("Connecting Spotify...");
}

void loop() {
    if (millis() - lastUpdate > UPDATE_INTERVAL) {
        if (spotify.updateTrackInfo()) {
            const TrackInfo& track = spotify.getTrackInfo();
            ui.updatePlayer(track.title, track.artist, track.progressMs, track.durationMs, track.isPlaying, track.isLiked);

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

    // Handle Buttons
    if (checkButton(BTN_DOWN_PIN, 1)) {
        spotify.playPause();
        delay(500); // Give API time to update
        spotify.updateTrackInfo();
    }
    if (checkButton(BTN_RIGHT_PIN, 3)) {
        spotify.nextTrack();
        lastTrackId = ""; // Force album art reload
        delay(500);
    }
    if (checkButton(BTN_LEFT_PIN, 2)) {
        spotify.prevTrack();
        lastTrackId = ""; // Force album art reload
        delay(500);
    }
    if (checkButton(BTN_CENTER_PIN, 4)) {
        spotify.toggleLike();
        delay(500);
        spotify.updateTrackInfo(); // Update heart status
    }
}
