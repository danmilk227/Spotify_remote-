#pragma once

// Button Pins (ESP32-C3)
#define BTN_UP_PIN      0  // Menu
#define BTN_DOWN_PIN    1  // Play/Pause
#define BTN_LEFT_PIN    3  // Prev
#define BTN_RIGHT_PIN   4  // Next
#define BTN_CENTER_PIN  5  // Select / Like

// Display Pins are defined in platformio.ini for TFT_eSPI:
// MOSI = 7, SCLK = 6, CS = 10, DC = 20, RST = 21, BL = 8

// Spotify API Endpoints
#define SPOTIFY_API_BASE "https://api.spotify.com/v1"
#define SPOTIFY_TOKEN_URL "https://accounts.spotify.com/api/token"
