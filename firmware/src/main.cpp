/**
 * ============================================================================
 * iPodify Physical Remote v2.0 (iPod Classic Edition - 6 Buttons)
 * MCU: ESP32-C3 (4MB Flash) | Display: SSD1306 0.96" I2C (128x64)
 * Controls: 6 Tactile Buttons (Up, Down, Left, Right, Center, Menu)
 * Connectivity: BLE GATT (Nordic UART Service) <-> Android Companion App
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_sleep.h>

// ======================= РАСПИНОВКА (ESP32-C3) ==============================
// I2C OLED SSD1306
#define PIN_I2C_SDA      6
#define PIN_I2C_SCL      5

// 6 тактовых кнопок (второй контакт каждой кнопки на GND):
#define PIN_BTN_LEFT     0   // Лево  (PREV / Назад)
#define PIN_BTN_RIGHT    1   // Право (NEXT / Вперед)
#define PIN_BTN_UP       4   // Вверх (Прокрутка вверх / Громкость +)
#define PIN_BTN_DOWN     2   // Вниз  (Прокрутка вниз / Громкость -)
#define PIN_BTN_CENTER   3   // Центр (OK / Выбор / Play-Pause)
#define PIN_BTN_MENU     7   // Меню  (Назад / Выход)

// ======================= BLE UUIDs (Nordic UART) ============================
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // Телефон -> ESP32
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> Телефон
#define CHARACTERISTIC_UUID_OTA "6E400004-B5A3-F393-E0A9-E50E24DCCA9E" // Телефон -> ESP32 (OTA binary stream)

// ======================= ДИСПЛЕЙ SSD1306 128x64 =============================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);

// ======================= СОСТОЯНИЕ ПЛЕЕРА ===================================
enum ScreenId {
    SCR_MENU_MAIN = 0,
    SCR_MENU_DYNAMIC,
    SCR_MENU_EXTRAS,
    SCR_MENU_SETTINGS,
    SCR_NOW_PLAYING,
    SCR_ABOUT,
    SCR_CLOCK,
    SCR_STOPWATCH,
    SCR_GAME_BRICK,
    SCR_GAME_SNAKE,
    SCR_OTA
};

void pushScreen(ScreenId nextScr, const String& folderId = "", const String& title = "");
void popScreen();

struct MenuStackEntry {
    ScreenId screen;
    String folderId;
    String title;
    int selectedIdx;
};

struct AppState {
    bool bleConnected = false;
    int batteryPct = 95;
    bool showBatteryPct = true;
    bool invertScreen = false;
    uint8_t brightnessLevel = 2; // 0: Low, 1: Med, 2: High

    // Playback state
    bool isPlaying = true;
    bool isLiked = true;
    bool shuffle = false;
    int repeatMode = 0; // 0: Off, 1: All, 2: One
    int sleepMinutes = 0;
    uint32_t sleepStartMs = 0;

    int queueIdx = 1;
    int queueTotal = 42;
    int positionSec = 28;
    int durationSec = 226;
    int volumePct = 65;

    String trackTitle = "Animal I Have Become";
    String trackArtist = "Three Days Grace";
    String trackAlbum = "One-X";

    // Overlays in Now Playing
    bool isScrubbing = false;
    int scrubPositionSec = 0;
    uint32_t scrubTimeoutMs = 0;

    bool showVolumeOverlay = false;
    uint32_t volumeTimeoutMs = 0;

    // Navigation
    ScreenId currentScreen = SCR_MENU_MAIN;
    String currentFolderId = "main";
    String currentMenuTitle = "iPod";
    int selectedIdx = 0;
    MenuStackEntry stack[8];
    int stackDepth = 0;

    // Toast & A-Z Popup
    String miniToastText = "";
    uint32_t miniToastUntilMs = 0;
    char azToastChar = 0;
    uint32_t azToastUntilMs = 0;
} state;

// Динамический буфер текущего открытого списка (Playlists, Songs и т.д.)
#define MAX_DYNAMIC_ITEMS 40
String dynamicItems[MAX_DYNAMIC_ITEMS];
int dynamicCount = 0;

// Демо-списки (работают до подключения к Android-приложению)
const char* DEMO_PLAYLISTS[] = {"My Wave", "Cyberpunk 2077 OST", "NERV Synchro Mix", "Late Night Synth", "Workout Heavy"};
const char* DEMO_ARTISTS[]   = {"Three Days Grace", "Justice", "Pholo", "Phelian", "Tame Impala", "Shiro Sagisu"};
const char* DEMO_ALBUMS[]    = {"One-X", "Hyperdrama", "Neon Genesis OST", "Edgerunners OST", "Currents"};
const char* DEMO_SONGS[]     = {
    "A Cruel Angel's Thesis", "Animal I Have Become", "Flower In A Hurricane",
    "I Really Want to Stay", "Kick Back", "Komm, susser Tod",
    "Never Too Late", "Night City Aliens", "pain", "Riot", "Sadness and Sorrow"
};

const char* MAIN_MENU_LABELS[] = {
    "Playlists", "Artists", "Albums", "Songs",
    "Top Tracks", "Recently Played", "Extras",
    "Settings", "Shuffle Songs", "Now Playing"
};
const int MAIN_MENU_COUNT = 10;

const char* EXTRAS_MENU_LABELS[] = {
    "Clock", "Stopwatch", "Brick (Breakout)", "Snake (Retro)", "Sleep Timer"
};
const int EXTRAS_MENU_COUNT = 5;

const char* SETTINGS_MENU_LABELS[] = {
    "About", "Shuffle", "Repeat", "Invert OLED", "Brightness", "Battery %", "Sync Library"
};
const int SETTINGS_MENU_COUNT = 7;

// ======================= BLE И OTA СЕРВЕР ===================================
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
BLECharacteristic* pOtaCharacteristic = nullptr;

SemaphoreHandle_t bleMutex = nullptr;
String pendingBlePacket = "";
bool hasNewBlePacket = false;

// Состояние OTA-прошивки по Bluetooth
bool isOtaUpdating = false;
size_t otaTotalBytes = 0;
size_t otaWrittenBytes = 0;
int otaProgressPct = 0;
String otaStatusMsg = "";

uint32_t lastVolCmdMs = 0;

// Управление сном экрана и Deep Sleep
bool isDisplaySleeping = false;
uint32_t lastActivityMs = 0;
const uint32_t SCREEN_TIMEOUT_MS = 10000; // 10 секунд бездействия

void enterDeepSleep() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(22, 36, "Deep Sleep...");
    u8g2.sendBuffer();
    delay(500);
    u8g2.setPowerSave(1);

    // Пробуждение по кнопке MENU (GPIO 7) или кнопке CENTER (GPIO 3)
    esp_deep_sleep_enable_gpio_wakeup((1ULL << PIN_BTN_MENU) | (1ULL << PIN_BTN_CENTER), ESP_GPIO_WAKEUP_GPIO_LOW);
    Serial.println("Entering Deep Sleep...");
    esp_deep_sleep_start();
}

void sendBleCommand(const String& cmd) {
    if (state.bleConnected && pTxCharacteristic) {
        pTxCharacteristic->setValue(cmd.c_str());
        pTxCharacteristic->notify();
    }
}

void sendVolumeCommand(bool up) {
    uint32_t now = millis();
    if (now - lastVolCmdMs >= 100) {
        lastVolCmdMs = now;
        sendBleCommand(up ? "CMD:VOL_UP" : "CMD:VOL_DOWN");
    }
}

void showToast(const String& msg, uint32_t durationMs = 1300) {
    state.miniToastText = msg;
    state.miniToastUntilMs = millis() + durationMs;
}

void showAlphabetPopup(char c) {
    state.azToastChar = toupper(c);
    state.azToastUntilMs = millis() + 600;
}

// Парсер входящих пакетов от Android-приложения
void parseIncomingBlePacket(const String& raw) {
    lastActivityMs = millis();
    if (isDisplaySleeping) {
        isDisplaySleeping = false;
        u8g2.setPowerSave(0);
    }

    if (raw.startsWith("OTA_START:")) {
        otaTotalBytes = raw.substring(10).toInt();
        otaWrittenBytes = 0;
        otaProgressPct = 0;
        otaStatusMsg = "Preparing OTA...";
        if (otaTotalBytes > 0 && Update.begin(otaTotalBytes, U_FLASH)) {
            isOtaUpdating = true;
            otaStatusMsg = "Flashing...";
            pushScreen(SCR_OTA, "ota", "Firmware OTA");
            sendBleCommand("OTA_ACK:START");
        } else {
            isOtaUpdating = false;
            otaStatusMsg = "OTA Init Failed";
            sendBleCommand("OTA_ERR:INIT");
        }
        return;
    } else if (raw == "OTA_END") {
        if (isOtaUpdating) {
            if (Update.end(true)) {
                otaProgressPct = 100;
                otaStatusMsg = "Success! Rebooting...";
                sendBleCommand("OTA_ACK:DONE");
                delay(700);
                ESP.restart();
            } else {
                isOtaUpdating = false;
                otaStatusMsg = "OTA Err: " + String(Update.getError());
                sendBleCommand("OTA_ERR:END");
            }
        }
        return;
    } else if (raw == "OTA_ABORT") {
        if (isOtaUpdating) {
            Update.abort();
            isOtaUpdating = false;
            otaStatusMsg = "Aborted";
            popScreen();
        }
        return;
    }

    if (raw.startsWith("NP|")) {
        // Формат: NP|play|like|shuf|rep|qIdx|qTot|pos|dur|vol|title|artist|album
        int idx[12];
        int pos = 0;
        for (int i = 0; i < 12; i++) {
            idx[i] = raw.indexOf('|', pos);
            if (idx[i] == -1) return;
            pos = idx[i] + 1;
        }
        state.isPlaying   = raw.substring(idx[0] + 1, idx[1]).toInt() != 0;
        state.isLiked     = raw.substring(idx[1] + 1, idx[2]).toInt() != 0;
        state.shuffle     = raw.substring(idx[2] + 1, idx[3]).toInt() != 0;
        state.repeatMode  = raw.substring(idx[3] + 1, idx[4]).toInt();
        state.queueIdx    = raw.substring(idx[4] + 1, idx[5]).toInt();
        state.queueTotal  = raw.substring(idx[5] + 1, idx[6]).toInt();
        if (!state.isScrubbing) {
            state.positionSec = raw.substring(idx[6] + 1, idx[7]).toInt();
        }
        state.durationSec = raw.substring(idx[7] + 1, idx[8]).toInt();
        state.volumePct   = raw.substring(idx[8] + 1, idx[9]).toInt();
        state.trackTitle  = raw.substring(idx[9] + 1, idx[10]);
        state.trackArtist = raw.substring(idx[10] + 1, idx[11]);
        state.trackAlbum  = raw.substring(idx[11] + 1);
    } else if (raw.startsWith("MN|")) {
        // Формат: MN|folderId|offset|Item1|Item2|...
        int p1 = raw.indexOf('|', 3);
        if (p1 == -1) return;
        int p2 = raw.indexOf('|', p1 + 1);
        if (p2 == -1) return;

        int offset = raw.substring(p1 + 1, p2).toInt();
        if (offset == 0) dynamicCount = 0;

        int cur = p2 + 1;
        while (cur < (int)raw.length() && dynamicCount < MAX_DYNAMIC_ITEMS) {
            int nextPipe = raw.indexOf('|', cur);
            if (nextPipe == -1) {
                dynamicItems[dynamicCount++] = raw.substring(cur);
                break;
            } else {
                dynamicItems[dynamicCount++] = raw.substring(cur, nextPipe);
                cur = nextPipe + 1;
            }
        }
    }
}

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pSrv) override {
        state.bleConnected = true;
        showToast("BLE Connected");
    }
    void onDisconnect(BLEServer* pSrv) override {
        state.bleConnected = false;
        showToast("BLE Disconnected");
        BLEDevice::startAdvertising();
    }
};

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        String rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0 && bleMutex) {
            if (xSemaphoreTake(bleMutex, (TickType_t)10) == pdTRUE) {
                pendingBlePacket = rxValue;
                hasNewBlePacket = true;
                xSemaphoreGive(bleMutex);
            }
        }
    }
};

class OtaCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        if (!isOtaUpdating) return;
        uint8_t* data = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();
        if (data && len > 0) {
            size_t written = Update.write(data, len);
            if (written > 0) {
                otaWrittenBytes += written;
                if (otaTotalBytes > 0) {
                    otaProgressPct = (int)((otaWrittenBytes * 100ULL) / otaTotalBytes);
                }
            }
        }
    }
};

void initBLE() {
    BLEDevice::init("iPodify 3G");
    BLEDevice::setMTU(512);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new RxCallbacks());

    pOtaCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_OTA,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pOtaCharacteristic->setCallbacks(new OtaCallbacks());

    pService->start();
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
}

// ======================= ИГРЫ И СЕКУНДОМЕР ==================================
// 1. Секундомер
struct Stopwatch {
    bool running = false;
    uint32_t startMs = 0;
    uint32_t elapsedMs = 0;
    uint32_t laps[3] = {0, 0, 0};
    int lapCount = 0;
} sw;

// 2. Brick (Breakout)
struct BrickGame {
    float paddleX = 50;
    const int paddleW = 24;
    float ballX = 64, ballY = 45;
    float vx = 1.6, vy = -1.6;
    int score = 0;
    int lives = 3;
    bool bricks[3][8];

    void reset() {
        paddleX = 52;
        ballX = 64; ballY = 48;
        vx = 1.5; vy = -1.5;
        score = 0; lives = 3;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 8; c++)
                bricks[r][c] = true;
    }
} brick;

// 3. Snake (Змейка)
struct SnakeGame {
    int x[64], y[64];
    int len = 4;
    int dir = 0; // 0:Right, 1:Down, 2:Left, 3:Up
    int foodX = 10, foodY = 6;
    int score = 0;
    uint32_t lastStepMs = 0;

    void spawnFood() {
        foodX = random(1, 30);
        foodY = random(4, 14);
    }
    void reset() {
        len = 4; dir = 0; score = 0;
        for (int i = 0; i < len; i++) { x[i] = 10 - i; y[i] = 8; }
        spawnFood();
    }
} snake;

// ======================= НАВИГАЦИЯ И ДЕЙСТВИЯ ===============================
void pushScreen(ScreenId nextScr, const String& folderId, const String& title) {
    if (state.stackDepth < 8) {
        state.stack[state.stackDepth++] = {
            state.currentScreen, state.currentFolderId, state.currentMenuTitle, state.selectedIdx
        };
    }
    state.currentScreen = nextScr;
    state.currentFolderId = folderId;
    state.currentMenuTitle = title;
    state.selectedIdx = 0;
}

void popScreen() {
    if (isOtaUpdating) return;
    if (state.currentScreen == SCR_NOW_PLAYING && state.isScrubbing) {
        state.isScrubbing = false;
        return;
    }
    if (state.stackDepth > 0) {
        MenuStackEntry prev = state.stack[--state.stackDepth];
        state.currentScreen = prev.screen;
        state.currentFolderId = prev.folderId;
        state.currentMenuTitle = prev.title;
        state.selectedIdx = prev.selectedIdx;
    }
}

void loadDynamicFolder(const String& folderId, const String& title) {
    pushScreen(SCR_MENU_DYNAMIC, folderId, title);
    dynamicCount = 0;

    // Сразу заполняем дефолтными списками — экран НИКОГДА не застрянет на "Loading..."
    if (folderId == "playlists") {
        for (auto s : DEMO_PLAYLISTS) dynamicItems[dynamicCount++] = s;
    } else if (folderId == "artists") {
        for (auto s : DEMO_ARTISTS) dynamicItems[dynamicCount++] = s;
    } else if (folderId == "albums") {
        for (auto s : DEMO_ALBUMS) dynamicItems[dynamicCount++] = s;
    } else {
        for (auto s : DEMO_SONGS) dynamicItems[dynamicCount++] = s;
    }

    if (state.bleConnected) {
        sendBleCommand("GET:" + folderId + ":0");
    }
}

int getCurrentMenuTotal() {
    if (state.currentScreen == SCR_MENU_MAIN) return MAIN_MENU_COUNT;
    if (state.currentScreen == SCR_MENU_EXTRAS) return EXTRAS_MENU_COUNT;
    if (state.currentScreen == SCR_MENU_SETTINGS) return SETTINGS_MENU_COUNT;
    if (state.currentScreen == SCR_MENU_DYNAMIC) return dynamicCount;
    return 0;
}

String getCurrentMenuItemLabel(int index) {
    if (state.currentScreen == SCR_MENU_MAIN) return MAIN_MENU_LABELS[index];
    if (state.currentScreen == SCR_MENU_EXTRAS) return EXTRAS_MENU_LABELS[index];
    if (state.currentScreen == SCR_MENU_SETTINGS) return SETTINGS_MENU_LABELS[index];
    if (state.currentScreen == SCR_MENU_DYNAMIC && index < dynamicCount) return dynamicItems[index];
    return "";
}

void commitScrub() {
    if (!state.isScrubbing) return;
    state.positionSec = state.scrubPositionSec;
    state.isScrubbing = false;
    sendBleCommand("CMD:SEEK:" + String(state.positionSec));
    showToast("Seek: " + String(state.positionSec) + "s", 900);
}

// Обработка навигации кнопками ВВЕРХ / ВНИЗ
void handleNavScroll(int dir) {
    if (state.currentScreen == SCR_GAME_BRICK) {
        brick.paddleX = constrain(brick.paddleX + dir * 8, 2, 126 - brick.paddleW);
        return;
    }
    if (state.currentScreen == SCR_GAME_SNAKE) {
        if (dir > 0 && snake.dir != 3) snake.dir = 1;      // Вниз
        else if (dir < 0 && snake.dir != 1) snake.dir = 3; // Вверх
        return;
    }
    if (state.currentScreen == SCR_NOW_PLAYING) {
        if (state.isScrubbing) {
            state.scrubPositionSec = constrain(state.scrubPositionSec - dir * 5, 0, state.durationSec);
            state.scrubTimeoutMs = millis() + 1500;
        } else {
            // dir < 0 (ВВЕРХ) -> Громкость +, dir > 0 (ВНИЗ) -> Громкость -
            if (dir < 0) {
                state.volumePct = min(100, state.volumePct + 5);
                sendVolumeCommand(true);
            } else {
                state.volumePct = max(0, state.volumePct - 5);
                sendVolumeCommand(false);
            }
            state.showVolumeOverlay = true;
            state.volumeTimeoutMs = millis() + 1800;
        }
        return;
    }

    int total = getCurrentMenuTotal();
    if (total <= 0) return;

    int nextIdx = (state.selectedIdx + dir + total) % total;
    if (nextIdx != state.selectedIdx) {
        state.selectedIdx = nextIdx;
        if (state.currentScreen == SCR_MENU_DYNAMIC) {
            String lbl = getCurrentMenuItemLabel(nextIdx);
            if (lbl.length() > 0 && isAlphaNumeric(lbl[0])) {
                showAlphabetPopup(lbl[0]);
            }
        }
    }
}

void handleCenterShortPress() {
    if (state.currentScreen == SCR_STOPWATCH) {
        if (!sw.running) {
            sw.startMs = millis();
            sw.running = true;
        } else {
            sw.elapsedMs += millis() - sw.startMs;
            sw.running = false;
        }
        return;
    }

    if (state.currentScreen == SCR_NOW_PLAYING) {
        if (state.isScrubbing) {
            commitScrub();
        } else {
            state.isPlaying = !state.isPlaying;
            sendBleCommand("CMD:PLAY_PAUSE");
        }
        return;
    }

    if (state.currentScreen == SCR_ABOUT || state.currentScreen == SCR_CLOCK) {
        popScreen();
        return;
    }

    if (state.currentScreen == SCR_MENU_MAIN) {
        switch (state.selectedIdx) {
            case 0: loadDynamicFolder("playlists", "Playlists"); break;
            case 1: loadDynamicFolder("artists", "Artists"); break;
            case 2: loadDynamicFolder("albums", "Albums"); break;
            case 3: loadDynamicFolder("songs", "Songs"); break;
            case 4: loadDynamicFolder("top", "Top Tracks"); break;
            case 5: loadDynamicFolder("recent", "Recent"); break;
            case 6: pushScreen(SCR_MENU_EXTRAS, "extras", "Extras"); break;
            case 7: pushScreen(SCR_MENU_SETTINGS, "settings", "Settings"); break;
            case 8:
                state.shuffle = true;
                sendBleCommand("CMD:SHUFFLE_ALL");
                showToast("Shuffling Songs");
                pushScreen(SCR_NOW_PLAYING, "np", "Now Playing");
                break;
            case 9:
                pushScreen(SCR_NOW_PLAYING, "np", "Now Playing");
                break;
        }
        return;
    }

    if (state.currentScreen == SCR_MENU_EXTRAS) {
        switch (state.selectedIdx) {
            case 0: pushScreen(SCR_CLOCK, "clock", "Clock"); break;
            case 1: pushScreen(SCR_STOPWATCH, "sw", "Stopwatch"); break;
            case 2: brick.reset(); pushScreen(SCR_GAME_BRICK, "brick", "Brick"); break;
            case 3: snake.reset(); pushScreen(SCR_GAME_SNAKE, "snake", "Snake"); break;
            case 4: {
                const int opts[] = {0, 15, 30, 60};
                int next = 0;
                for (int i = 0; i < 4; i++) if (opts[i] == state.sleepMinutes) next = opts[(i + 1) % 4];
                state.sleepMinutes = next;
                state.sleepStartMs = millis();
                sendBleCommand("CMD:SLEEP:" + String(state.sleepMinutes));
                break;
            }
        }
        return;
    }

    if (state.currentScreen == SCR_MENU_SETTINGS) {
        switch (state.selectedIdx) {
            case 0: pushScreen(SCR_ABOUT, "about", "About"); break;
            case 1:
                state.shuffle = !state.shuffle;
                sendBleCommand("CMD:SHUFFLE:" + String(state.shuffle ? 1 : 0));
                break;
            case 2:
                state.repeatMode = (state.repeatMode + 1) % 3;
                sendBleCommand("CMD:REPEAT:" + String(state.repeatMode));
                break;
            case 3:
                state.invertScreen = !state.invertScreen;
                u8g2.sendF("c", state.invertScreen ? 0xA7 : 0xA6);
                break;
            case 4: {
                state.brightnessLevel = (state.brightnessLevel + 1) % 3;
                const uint8_t contrasts[] = {15, 110, 255};
                u8g2.setContrast(contrasts[state.brightnessLevel]);
                break;
            }
            case 5:
                state.showBatteryPct = !state.showBatteryPct;
                break;
            case 6:
                sendBleCommand("CMD:SYNC");
                showToast("Syncing Library...");
                break;
        }
        return;
    }

    if (state.currentScreen == SCR_MENU_DYNAMIC) {
        if (dynamicCount == 0) return;
        String chosen = dynamicItems[state.selectedIdx];
        sendBleCommand("PLAY_ITEM:" + state.currentFolderId + ":" + String(state.selectedIdx));
        state.trackTitle = chosen;
        state.positionSec = 0;
        state.isPlaying = true;
        pushScreen(SCR_NOW_PLAYING, "np", "Now Playing");
    }
}

void handleCenterLongPress() {
    if (state.currentScreen == SCR_NOW_PLAYING) {
        state.isLiked = !state.isLiked;
        sendBleCommand("CMD:LIKE");
        showToast(state.isLiked ? "Saved to Liked" : "Removed Like");
    }
}

// ======================= ОПРОС 6 КНОПОК =====================================
struct BtnTracker {
    uint8_t pin;
    bool lastLevel = HIGH;
    uint32_t pressStartMs = 0;
    bool longFired = false;
    uint32_t lastRepeatMs = 0;
    bool repeatFired = false;
};

BtnTracker btns[6] = {
    {PIN_BTN_LEFT},
    {PIN_BTN_RIGHT},
    {PIN_BTN_UP},
    {PIN_BTN_DOWN},
    {PIN_BTN_CENTER},
    {PIN_BTN_MENU}
};

void onButtonShortPress(uint8_t pin) {
    if (isOtaUpdating) return;
    switch (pin) {
        case PIN_BTN_MENU:
            popScreen();
            break;

        case PIN_BTN_UP:
            handleNavScroll(-1);
            break;

        case PIN_BTN_DOWN:
            handleNavScroll(1);
            break;

        case PIN_BTN_LEFT:
            if (state.currentScreen == SCR_STOPWATCH && !sw.running) {
                sw.elapsedMs = 0;
                sw.lapCount = 0;
                sw.laps[0] = sw.laps[1] = sw.laps[2] = 0;
            } else if (state.currentScreen == SCR_GAME_BRICK) {
                brick.paddleX = constrain(brick.paddleX - 10, 2, 126 - brick.paddleW);
            } else if (state.currentScreen == SCR_GAME_SNAKE) {
                if (snake.dir != 0) snake.dir = 2; // Влево
            } else if (state.currentScreen == SCR_NOW_PLAYING) {
                sendBleCommand("CMD:PREV");
                state.positionSec = 0;
            } else {
                popScreen();
            }
            break;

        case PIN_BTN_RIGHT:
            if (state.currentScreen == SCR_STOPWATCH && sw.running) {
                uint32_t lap = sw.elapsedMs + (millis() - sw.startMs);
                sw.laps[0] = sw.laps[1];
                sw.laps[1] = sw.laps[2];
                sw.laps[2] = lap;
                sw.lapCount++;
            } else if (state.currentScreen == SCR_GAME_BRICK) {
                brick.paddleX = constrain(brick.paddleX + 10, 2, 126 - brick.paddleW);
            } else if (state.currentScreen == SCR_GAME_SNAKE) {
                if (snake.dir != 2) snake.dir = 0; // Вправо
            } else if (state.currentScreen == SCR_NOW_PLAYING) {
                sendBleCommand("CMD:NEXT");
                state.positionSec = 0;
            } else {
                handleCenterShortPress();
            }
            break;

        case PIN_BTN_CENTER:
            handleCenterShortPress();
            break;
    }
}

void onButtonLongPress(uint8_t pin) {
    if (isOtaUpdating) return;

    if (pin == PIN_BTN_MENU) {
        enterDeepSleep();
        return;
    }

    if (state.currentScreen == SCR_NOW_PLAYING) {
        if (pin == PIN_BTN_CENTER) {
            handleCenterLongPress();
        } else if (pin == PIN_BTN_LEFT) {
            state.positionSec = max(0, state.positionSec - 10);
            sendBleCommand("CMD:SEEK:" + String(state.positionSec));
            showToast("Seek: -" + String(state.positionSec) + "s", 800);
        } else if (pin == PIN_BTN_RIGHT) {
            state.positionSec = min(state.durationSec, state.positionSec + 10);
            sendBleCommand("CMD:SEEK:" + String(state.positionSec));
            showToast("Seek: +" + String(state.positionSec) + "s", 800);
        }
    }
}

void onButtonRepeat(uint8_t pin) {
    if (isOtaUpdating) return;
    if (pin == PIN_BTN_UP) {
        handleNavScroll(-1);
    } else if (pin == PIN_BTN_DOWN) {
        handleNavScroll(1);
    } else if (pin == PIN_BTN_LEFT && state.currentScreen == SCR_GAME_BRICK) {
        brick.paddleX = constrain(brick.paddleX - 6, 2, 126 - brick.paddleW);
    } else if (pin == PIN_BTN_RIGHT && state.currentScreen == SCR_GAME_BRICK) {
        brick.paddleX = constrain(brick.paddleX + 6, 2, 126 - brick.paddleW);
    }
}

void pollButtons() {
    uint32_t now = millis();
    for (int i = 0; i < 6; i++) {
        bool lvl = digitalRead(btns[i].pin);
        if (lvl == LOW && btns[i].lastLevel == HIGH) {
            btns[i].pressStartMs = now;
            btns[i].longFired = false;
            btns[i].repeatFired = false;
            btns[i].lastRepeatMs = now;

            if (isDisplaySleeping) {
                // Первое нажатие при спящем экране только будит его
                isDisplaySleeping = false;
                u8g2.setPowerSave(0);
                lastActivityMs = now;
                btns[i].lastLevel = lvl;
                continue;
            }
            lastActivityMs = now;
        } else if (lvl == LOW) {
            lastActivityMs = now;
            if (!btns[i].longFired && (now - btns[i].pressStartMs > 550)) {
                btns[i].longFired = true;
                onButtonLongPress(btns[i].pin);
            }
            if (now - btns[i].pressStartMs > 350 && (now - btns[i].lastRepeatMs > 160)) {
                btns[i].lastRepeatMs = now;
                btns[i].repeatFired = true;
                onButtonRepeat(btns[i].pin);
            }
        } else if (lvl == HIGH && btns[i].lastLevel == LOW) {
            lastActivityMs = now;
            if (now - btns[i].pressStartMs > 25 && !btns[i].longFired && !btns[i].repeatFired) {
                onButtonShortPress(btns[i].pin);
            }
        }
        btns[i].lastLevel = lvl;
    }
}

// ======================= ОТРИСОВКА ИНТЕРФЕЙСА (U8G2) ========================
String formatSec(int totalSec) {
    if (totalSec < 0) totalSec = 0;
    int m = totalSec / 60;
    int s = totalSec % 60;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return String(buf);
}

void drawStatusBar() {
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);

    // Заголовок слева
    String title = state.currentMenuTitle;
    if (title.length() > 11) title = title.substring(0, 11);
    u8g2.drawUTF8(2, 9, title.c_str());

    // Статус BLE
    if (state.bleConnected) {
        u8g2.drawStr(72, 9, "B");
    }

    // Иконка Play / Pause
    if (state.isPlaying) {
        u8g2.drawTriangle(82, 2, 82, 8, 87, 5);
    } else {
        u8g2.drawBox(82, 2, 2, 7);
        u8g2.drawBox(86, 2, 2, 7);
    }

    // Процент заряда и батарейка
    if (state.showBatteryPct) {
        u8g2.setFont(u8g2_font_5x7_tf);
        char bStr[6];
        snprintf(bStr, sizeof(bStr), "%d%%", state.batteryPct);
        u8g2.drawStr(91, 8, bStr);
    }

    u8g2.drawFrame(112, 2, 13, 7);
    u8g2.drawBox(125, 4, 2, 3);
    int fillW = map(constrain(state.batteryPct, 0, 100), 0, 100, 1, 9);
    u8g2.drawBox(114, 4, fillW, 3);

    u8g2.drawHLine(0, 11, 128);
}

String getRightValueForItem(int idx) {
    if (state.currentScreen == SCR_MENU_EXTRAS && idx == 4) {
        return state.sleepMinutes == 0 ? "Off" : (String(state.sleepMinutes) + "m");
    }
    if (state.currentScreen == SCR_MENU_SETTINGS) {
        switch (idx) {
            case 1: return state.shuffle ? "Songs" : "Off";
            case 2: return state.repeatMode == 0 ? "Off" : (state.repeatMode == 1 ? "All" : "One");
            case 3: return state.invertScreen ? "On" : "Off";
            case 4: return state.brightnessLevel == 0 ? "Low" : (state.brightnessLevel == 1 ? "Med" : "Max");
            case 5: return state.showBatteryPct ? "On" : "Off";
            default: return "";
        }
    }
    return ">";
}

void drawMenuScreen() {
    int total = getCurrentMenuTotal();
    const int visibleRows = 4;
    int firstRow = 0;
    if (state.selectedIdx >= visibleRows) {
        firstRow = state.selectedIdx - visibleRows + 1;
    }

    u8g2.setFont(u8g2_font_6x12_t_cyrillic);

    for (int r = 0; r < visibleRows; r++) {
        int itemIdx = firstRow + r;
        if (itemIdx >= total) break;

        int yTop = 13 + r * 12;
        bool isSelected = (itemIdx == state.selectedIdx);

        if (isSelected) {
            u8g2.setDrawColor(1);
            u8g2.drawBox(0, yTop, total > visibleRows ? 121 : 128, 12);
            u8g2.setDrawColor(0);
        } else {
            u8g2.setDrawColor(1);
        }

        String label = getCurrentMenuItemLabel(itemIdx);
        if (label.length() > 16) label = label.substring(0, 16);
        u8g2.drawUTF8(3, yTop + 10, label.c_str());

        String rightVal = getRightValueForItem(itemIdx);
        if (rightVal.length() > 0) {
            int rw = u8g2.getUTF8Width(rightVal.c_str());
            int rightEdge = (total > visibleRows) ? 119 : 125;
            u8g2.drawUTF8(rightEdge - rw, yTop + 10, rightVal.c_str());
        }
    }

    // Скроллбар iPod справа
    u8g2.setDrawColor(1);
    if (total > visibleRows) {
        u8g2.drawFrame(123, 13, 5, 50);
        int thumbH = max(8, (visibleRows * 48) / total);
        int maxTop = 48 - thumbH;
        int thumbY = 14 + (state.selectedIdx * maxTop) / max(1, total - 1);
        u8g2.drawBox(124, thumbY, 3, thumbH);
    }
}

void drawNowPlayingScreen() {
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_5x8_tf);

    // 1. Верхняя мета-строка: ♥ 1 of 42  |  [Scrub] / Shuf / Rep
    String idxStr = String(state.queueIdx) + " of " + String(state.queueTotal);
    int leftX = 2;
    if (state.isLiked) {
        u8g2.drawGlyph(2, 21, 0x2665); // Сердечко (или символ)
        u8g2.drawBox(2, 16, 5, 4);
        leftX = 10;
    }
    u8g2.drawStr(leftX, 21, idxStr.c_str());

    if (state.isScrubbing) {
        u8g2.drawStr(88, 21, "[Scrub]");
    } else {
        String modes = "";
        if (state.shuffle) modes += "SHF ";
        if (state.repeatMode == 1) modes += "R:ALL";
        else if (state.repeatMode == 2) modes += "R:1";
        u8g2.drawStr(126 - u8g2.getStrWidth(modes.c_str()), 21, modes.c_str());
    }

    // 2. Название трека (с плавной бегущей строкой, если длинное)
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    int titleW = u8g2.getUTF8Width(state.trackTitle.c_str());
    if (titleW <= 124) {
        u8g2.drawUTF8((128 - titleW) / 2, 34, state.trackTitle.c_str());
    } else {
        int scrollRange = titleW - 116;
        int offset = (millis() / 45) % (scrollRange + 30);
        int drawX = 4 - constrain(offset - 10, 0, scrollRange);
        u8g2.drawUTF8(drawX, 34, state.trackTitle.c_str());
    }

    // 3. Исполнитель
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    String artist = state.trackArtist;
    if (artist.length() > 24) artist = artist.substring(0, 24);
    int artW = u8g2.getUTF8Width(artist.c_str());
    u8g2.drawUTF8(max(2, (128 - artW) / 2), 45, artist.c_str());

    // 4. Нижняя зона: Громкость ИЛИ Прогресс-бар / Scrubbing
    if (state.showVolumeOverlay) {
        u8g2.drawStr(4, 58, "VOL");
        u8g2.drawFrame(24, 52, 82, 7);
        int vw = map(state.volumePct, 0, 100, 0, 78);
        u8g2.drawBox(26, 54, vw, 3);
        char vBuf[8];
        snprintf(vBuf, sizeof(vBuf), "%d%%", state.volumePct);
        u8g2.drawStr(109, 58, vBuf);
    } else {
        int activePos = state.isScrubbing ? state.scrubPositionSec : state.positionSec;
        int pct = state.durationSec > 0 ? constrain((activePos * 100) / state.durationSec, 0, 100) : 0;

        u8g2.drawFrame(4, 49, 120, 6);
        if (state.isScrubbing) {
            int diamondX = 6 + map(pct, 0, 100, 0, 114);
            u8g2.drawBox(diamondX - 2, 48, 5, 8);
        } else {
            int pw = map(pct, 0, 100, 0, 116);
            u8g2.drawBox(6, 51, pw, 2);
        }

        String curT = formatSec(activePos);
        String remT = "-" + formatSec(max(0, state.durationSec - activePos));
        u8g2.drawStr(4, 63, curT.c_str());
        u8g2.drawStr(124 - u8g2.getStrWidth(remT.c_str()), 63, remT.c_str());
    }
}

void drawAboutScreen() {
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(4, 23, "Dan's iPod 3G Remote");
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr(4, 34, ("Songs: " + String(state.queueTotal)).c_str());
    u8g2.drawStr(4, 44, ("BLE: " + String(state.bleConnected ? "Connected" : "Advertising")).c_str());
    u8g2.drawStr(4, 54, "HW: ESP32-C3 (6-btn)");
    u8g2.drawStr(4, 63, "FW: v2.0 (GATT NUS)");
}

void drawClockScreen() {
    u8g2.setDrawColor(1);
    uint32_t totalSec = millis() / 1000;
    int h = (totalSec / 3600) % 24;
    int m = (totalSec / 60) % 60;
    int s = totalSec % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);

    u8g2.setFont(u8g2_font_logisoso18_tn);
    int w = u8g2.getStrWidth(buf);
    u8g2.drawStr((128 - w) / 2, 38, buf);

    u8g2.setFont(u8g2_font_5x8_tf);
    String slp = state.sleepMinutes > 0 ? ("Sleep Timer: " + String(state.sleepMinutes) + "m") : "Sleep Timer: Off";
    int swW = u8g2.getStrWidth(slp.c_str());
    u8g2.drawStr((128 - swW) / 2, 56, slp.c_str());
}

void drawStopwatchScreen() {
    u8g2.setDrawColor(1);
    uint32_t totalMs = sw.running ? (sw.elapsedMs + (millis() - sw.startMs)) : sw.elapsedMs;
    uint32_t cs = (totalMs / 10) % 100;
    uint32_t s  = (totalMs / 1000) % 60;
    uint32_t m  = totalMs / 60000;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", m, s, cs);
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(28, 26, buf);

    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr(4, 36, "OK:Start/Stop NEXT:Lap");
    for (int i = 0; i < 3; i++) {
        if (sw.laps[i] > 0) {
            char lBuf[24];
            snprintf(lBuf, sizeof(lBuf), "Lap: %lu.%02lus", sw.laps[i] / 1000, (sw.laps[i] / 10) % 100);
            u8g2.drawStr(4, 46 + i * 8, lBuf);
        }
    }
}

void updateAndDrawBrickGame() {
    u8g2.setDrawColor(1);
    brick.ballX += brick.vx;
    brick.ballY += brick.vy;

    if (brick.ballX <= 2 || brick.ballX >= 125) brick.vx *= -1;
    if (brick.ballY <= 14) brick.vy *= -1;

    // Отскок от ракетки
    if (brick.ballY >= 58 && brick.ballY <= 61 &&
        brick.ballX >= brick.paddleX && brick.ballX <= brick.paddleX + brick.paddleW) {
        brick.vy = -fabs(brick.vy);
        float offset = (brick.ballX - (brick.paddleX + brick.paddleW / 2.0f)) / (brick.paddleW / 2.0f);
        brick.vx = offset * 2.2f;
    }

    // Кирпичи
    bool anyAlive = false;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 8; c++) {
            if (!brick.bricks[r][c]) continue;
            anyAlive = true;
            int bx = 4 + c * 15;
            int by = 16 + r * 6;
            u8g2.drawBox(bx, by, 13, 4);
            if (brick.ballX >= bx && brick.ballX <= bx + 13 &&
                brick.ballY >= by && brick.ballY <= by + 4) {
                brick.bricks[r][c] = false;
                brick.vy *= -1;
                brick.score += 10;
            }
        }
    }
    if (!anyAlive) brick.reset();

    if (brick.ballY > 64) {
        brick.lives--;
        if (brick.lives <= 0) brick.reset();
        else {
            brick.ballX = brick.paddleX + brick.paddleW / 2;
            brick.ballY = 55;
            brick.vy = -1.5;
        }
    }

    u8g2.drawBox((int)brick.paddleX, 60, brick.paddleW, 3);
    u8g2.drawBox((int)brick.ballX - 1, (int)brick.ballY - 1, 3, 3);
}

void updateAndDrawSnakeGame() {
    uint32_t now = millis();
    if (now - snake.lastStepMs >= 160) {
        snake.lastStepMs = now;
        const int dx[] = {1, 0, -1, 0};
        const int dy[] = {0, 1, 0, -1};
        int nx = snake.x[0] + dx[snake.dir];
        int ny = snake.y[0] + dy[snake.dir];

        if (nx < 0 || nx >= 32 || ny < 3 || ny >= 16) {
            snake.reset();
        } else {
            for (int i = snake.len - 1; i > 0; i--) {
                snake.x[i] = snake.x[i - 1];
                snake.y[i] = snake.y[i - 1];
                if (snake.x[i] == nx && snake.y[i] == ny) snake.reset();
            }
            snake.x[0] = nx;
            snake.y[0] = ny;

            if (nx == snake.foodX && ny == snake.foodY) {
                if (snake.len < 60) snake.len++;
                snake.score += 10;
                snake.spawnFood();
            }
        }
    }

    u8g2.setDrawColor(1);
    u8g2.drawFrame(snake.foodX * 4, snake.foodY * 4, 4, 4);
    for (int i = 0; i < snake.len; i++) {
        u8g2.drawBox(snake.x[i] * 4, snake.y[i] * 4, 3, 3);
    }
}

void drawOverlays() {
    uint32_t now = millis();

    // Всплывающая буква быстрого скролла A-Z
    if (now < state.azToastUntilMs && state.azToastChar != 0) {
        u8g2.setDrawColor(1);
        u8g2.drawRBox(48, 20, 32, 30, 4);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_logisoso18_tr);
        char s[2] = {state.azToastChar, 0};
        int cw = u8g2.getStrWidth(s);
        u8g2.drawStr(48 + (32 - cw) / 2, 44, s);
        u8g2.setDrawColor(1);
    }

    // Мини-тост уведомлений снизу
    if (now < state.miniToastUntilMs && state.miniToastText.length() > 0) {
        u8g2.setFont(u8g2_font_5x8_tf);
        int tw = u8g2.getStrWidth(state.miniToastText.c_str()) + 10;
        int tx = max(2, (128 - tw) / 2);
        u8g2.setDrawColor(1);
        u8g2.drawRBox(tx, 51, tw, 12, 3);
        u8g2.setDrawColor(0);
        u8g2.drawStr(tx + 5, 60, state.miniToastText.c_str());
        u8g2.setDrawColor(1);
    }
}

void drawOtaScreen() {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(14, 18, "Firmware Update");

    u8g2.drawFrame(14, 28, 100, 10);
    int barW = map(constrain(otaProgressPct, 0, 100), 0, 100, 0, 96);
    u8g2.drawBox(16, 30, barW, 6);

    char pBuf[16];
    snprintf(pBuf, sizeof(pBuf), "%d%%", otaProgressPct);
    int pw = u8g2.getUTF8Width(pBuf);
    u8g2.drawStr((128 - pw) / 2, 48, pBuf);

    if (otaStatusMsg.length() > 0) {
        int sw = u8g2.getUTF8Width(otaStatusMsg.c_str());
        u8g2.drawStr(max(2, (128 - sw) / 2), 60, otaStatusMsg.c_str());
    }
}

// ======================= SETUP & LOOP =======================================
uint32_t lastSecondTickMs = 0;
uint32_t lastFrameMs = 0;

void setup() {
    Serial.begin(115200);

    for (int i = 0; i < 6; i++) {
        pinMode(btns[i].pin, INPUT_PULLUP);
    }

    bleMutex = xSemaphoreCreateMutex();
    lastActivityMs = millis();

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setTimeOut(50);   // Защита от зависания шины I2C при помехах
    Wire.setClock(400000); // Разгон I2C со 100 кГц до 400 кГц
    u8g2.begin();
    u8g2.enableUTF8Print();
    u8g2.setContrast(255);

    initBLE();
    brick.reset();
    snake.reset();
}

void loop() {
    // Безопасная обработка входящих пакетов BLE в основном потоке (без порчи кучи)
    if (hasNewBlePacket && bleMutex) {
        String pkt = "";
        if (xSemaphoreTake(bleMutex, (TickType_t)5) == pdTRUE) {
            pkt = pendingBlePacket;
            hasNewBlePacket = false;
            xSemaphoreGive(bleMutex);
        }
        if (pkt.length() > 0) {
            parseIncomingBlePacket(pkt);
        }
    }

    pollButtons();

    uint32_t now = millis();

    // Автоотключение экрана через 10 секунд бездействия
    if (!isDisplaySleeping && !isOtaUpdating && (now - lastActivityMs >= SCREEN_TIMEOUT_MS)) {
        isDisplaySleeping = true;
        u8g2.setPowerSave(1);
    }

    // Таймауты режимов громкости и скраббинга
    if (state.showVolumeOverlay && now > state.volumeTimeoutMs) {
        state.showVolumeOverlay = false;
    }
    if (state.isScrubbing && now > state.scrubTimeoutMs) {
        commitScrub();
    }

    // Секундный тик прогресса песни и таймера сна
    if (now - lastSecondTickMs >= 1000) {
        lastSecondTickMs = now;
        if (state.isPlaying && !state.isScrubbing) {
            state.positionSec++;
            if (state.positionSec > state.durationSec) state.positionSec = 0;
        }
        if (state.sleepMinutes > 0 && (now - state.sleepStartMs >= (uint32_t)state.sleepMinutes * 60000UL)) {
            state.sleepMinutes = 0;
            state.isPlaying = false;
            sendBleCommand("CMD:PLAY_PAUSE");
            showToast("Sleep Timer Off");
        }
    }

    // Отрисовка кадра ~33 FPS (только если экран не спит)
    if (!isDisplaySleeping && (now - lastFrameMs >= 30)) {
        lastFrameMs = now;
        u8g2.clearBuffer();
        drawStatusBar();

        switch (state.currentScreen) {
            case SCR_MENU_MAIN:
            case SCR_MENU_DYNAMIC:
            case SCR_MENU_EXTRAS:
            case SCR_MENU_SETTINGS:
                drawMenuScreen();
                break;
            case SCR_NOW_PLAYING:
                drawNowPlayingScreen();
                break;
            case SCR_ABOUT:
                drawAboutScreen();
                break;
            case SCR_CLOCK:
                drawClockScreen();
                break;
            case SCR_STOPWATCH:
                drawStopwatchScreen();
                break;
            case SCR_GAME_BRICK:
                updateAndDrawBrickGame();
                break;
            case SCR_GAME_SNAKE:
                updateAndDrawSnakeGame();
                break;
            case SCR_OTA:
                drawOtaScreen();
                break;
        }

        drawOverlays();
        u8g2.sendBuffer();
    }
}
