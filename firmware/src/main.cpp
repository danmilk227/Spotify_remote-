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
    SCR_FLASHLIGHT,
    SCR_EQUALIZER,
    SCR_PC_STREAM,
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
    int uiStyle = 1; // 0: Classic iPod, 1: Modern Split UI
    bool invertScreen = false;
    uint8_t brightnessLevel = 2; // 0: 25%, 1: 50%, 2: 75%, 3: 100%
    bool visualizerEnabled = true;
    int sleepTimeoutSeconds = 10;
    uint8_t realSpec[5] = {3, 4, 6, 4, 2};
    uint32_t lastSpecMs = 0; // 5, 10, 30, 0 (Never)

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
    "Queue", "Recently Played", "Extras",
    "Settings", "Shuffle Songs", "Now Playing"
};
const int MAIN_MENU_COUNT = 10;

const char* EXTRAS_MENU_LABELS[] = {
    "Clock", "Stopwatch", "Flashlight", "Equalizer", "PC Monitor Stream", "Brick (Breakout)", "Snake (Retro)", "Sleep Timer"
};
const int EXTRAS_MENU_COUNT = 8;

const char* SETTINGS_MENU_LABELS[] = {
    "About", "UI Style", "Screen Sleep", "Brightness", "Visualizer", "Shuffle", "Repeat", "Deep Sleep Now"
};
const int SETTINGS_MENU_COUNT = 8;

const char* EQ_PRESET_NAMES[] = {"Flat", "Bass Boost", "Acoustic", "Rock", "Electronic", "Vocal"};
const int8_t EQ_CURVES[6][5] = {
    {0, 0, 0, 0, 0},
    {6, 5, 2, 0, -1},
    {3, 2, 0, 2, 4},
    {5, 2, -1, 3, 5},
    {5, 4, 0, 2, 4},
    {-2, 0, 4, 3, 1}
};
int eqPresetIdx = 0;

// Буфер для монохромной обложки альбома (48x48 1-bit XBM = 288 байт)
uint8_t albumArtBitmap[288];
bool hasAlbumArt = false;
String b64ArtPart0 = "";

int decodeBase64(const char* input, size_t inputLen, uint8_t* output, size_t maxOut) {
    static const int8_t b64_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    size_t outLen = 0;
    uint32_t val = 0;
    int valb = -8;
    for (size_t i = 0; i < inputLen; i++) {
        uint8_t c = (uint8_t)input[i];
        if (c == '=') break;
        int8_t d = b64_table[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            if (outLen < maxOut) {
                output[outLen++] = (uint8_t)((val >> valb) & 0xFF);
            }
            valb -= 8;
        }
    }
    return outLen;
}

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
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(14, 28, "Deep Sleep...");
    u8g2.drawStr(4, 44, "Press Center to wake");
    u8g2.sendBuffer();

    // Ждем, пока пользователь отпустит кнопку MENU, чтобы избежать мгновенного пробуждения
    while (digitalRead(PIN_BTN_MENU) == LOW) {
        delay(10);
    }
    delay(300);
    u8g2.setPowerSave(1);

    // На ESP32-C3 пробуждение из Deep Sleep поддерживается ТОЛЬКО на RTC GPIO: 0, 1, 2, 3, 4, 5.
    // Пин MENU (GPIO 7) не является RTC-пином, поэтому пробуждаем по кнопкам:
    // CENTER (GPIO 3), UP (GPIO 4), DOWN (GPIO 2), LEFT (GPIO 0), RIGHT (GPIO 1).
    gpio_pullup_en((gpio_num_t)PIN_BTN_CENTER);
    gpio_pullup_en((gpio_num_t)PIN_BTN_UP);
    gpio_pullup_en((gpio_num_t)PIN_BTN_DOWN);
    gpio_pullup_en((gpio_num_t)PIN_BTN_LEFT);
    gpio_pullup_en((gpio_num_t)PIN_BTN_RIGHT);

    const uint64_t WAKEUP_RTC_MASK = (1ULL << PIN_BTN_CENTER) |
                                     (1ULL << PIN_BTN_UP) |
                                     (1ULL << PIN_BTN_DOWN) |
                                     (1ULL << PIN_BTN_LEFT) |
                                     (1ULL << PIN_BTN_RIGHT);

    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(WAKEUP_RTC_MASK, ESP_GPIO_WAKEUP_GPIO_LOW);
    Serial.printf("[SLEEP] esp_deep_sleep_enable_gpio_wakeup: %d\n", err);
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
    } else if (raw.startsWith("OTA_END")) {
        if (isOtaUpdating) {
            Serial.printf("[OTA] OTA_END received. Received: %u / %u bytes\n", otaWrittenBytes, otaTotalBytes);
            if (otaTotalBytes > 0 && otaWrittenBytes < otaTotalBytes) {
                isOtaUpdating = false;
                otaStatusMsg = "Err: Missed " + String(otaTotalBytes - otaWrittenBytes) + " B";
                Update.abort();
                sendBleCommand("OTA_ERR:INCOMPLETE");
                return;
            }

            if (Update.end(false)) { // false: ensure exact binary size and checksum match
                otaProgressPct = 100;
                otaStatusMsg = "Success! Rebooting...";
                sendBleCommand("OTA_ACK:DONE");
                delay(1000);
                ESP.restart();
            } else {
                isOtaUpdating = false;
                otaStatusMsg = "OTA Err: " + String(Update.getError());
                sendBleCommand("OTA_ERR:END_" + String(Update.getError()));
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
        String newTitle = raw.substring(idx[9] + 1, idx[10]);
        if (newTitle != state.trackTitle) {
            hasAlbumArt = false;
            b64ArtPart0 = "";
        }
        state.trackTitle  = newTitle;
        state.trackArtist = raw.substring(idx[10] + 1, idx[11]);
        state.trackAlbum  = raw.substring(idx[11] + 1);
    } else if (raw.startsWith("ART0:")) {
        b64ArtPart0 = raw.substring(5);
    } else if (raw.startsWith("ART1:")) {
        String fullB64 = b64ArtPart0 + raw.substring(5);
        int decoded = decodeBase64(fullB64.c_str(), fullB64.length(), albumArtBitmap, sizeof(albumArtBitmap));
        if (decoded == 288) {
            hasAlbumArt = true;
        }
    } else if (raw.startsWith("ART:")) {
        String b64 = raw.substring(4);
        int decoded = decodeBase64(b64.c_str(), b64.length(), albumArtBitmap, sizeof(albumArtBitmap));
        if (decoded == 288) {
            hasAlbumArt = true;
        }
    } else if (raw.startsWith("SPEC:")) {
        // Format: SPEC:v0,v1,v2,v3,v4
        int idx = 0;
        int cur = 5;
        while (cur < (int)raw.length() && idx < 5) {
            int comma = raw.indexOf(',', cur);
            if (comma == -1) {
                state.realSpec[idx++] = raw.substring(cur).toInt();
                break;
            } else {
                state.realSpec[idx++] = raw.substring(cur, comma).toInt();
                cur = comma + 1;
            }
        }
        state.lastSpecMs = millis();
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
    if (nextScr == SCR_FLASHLIGHT) {
        u8g2.setContrast(255);
    }
}

void popScreen() {
    if (isOtaUpdating) return;
    if (state.currentScreen == SCR_FLASHLIGHT) {
        const uint8_t contrasts[] = {25, 80, 160, 255};
        u8g2.setContrast(contrasts[state.brightnessLevel]);
    }
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
        if (state.currentScreen == SCR_MENU_DYNAMIC && state.bleConnected) {
            sendBleCommand("GET:" + state.currentFolderId + ":0");
        }
    }
}

void loadDynamicFolder(const String& folderId, const String& title) {
    pushScreen(SCR_MENU_DYNAMIC, folderId, title);
    dynamicCount = 0;

    if (folderId == "playlists") {
        for (auto s : DEMO_PLAYLISTS) dynamicItems[dynamicCount++] = s;
    } else if (folderId == "artists") {
        for (auto s : DEMO_ARTISTS) dynamicItems[dynamicCount++] = s;
    } else if (folderId == "albums") {
        for (auto s : DEMO_ALBUMS) dynamicItems[dynamicCount++] = s;
    } else if (folderId == "songs" || folderId == "top" || folderId == "recent" || folderId == "queue") {
        for (auto s : DEMO_SONGS) dynamicItems[dynamicCount++] = s;
    } else {
        dynamicItems[dynamicCount++] = "Loading tracks...";
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
    if (state.currentScreen == SCR_FLASHLIGHT) {
        popScreen();
        return;
    }
    if (state.currentScreen == SCR_EQUALIZER) {
        eqPresetIdx = (eqPresetIdx + dir + 6) % 6;
        sendBleCommand("CMD:EQ:" + String(eqPresetIdx));
        return;
    }
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
    if (state.currentScreen == SCR_FLASHLIGHT) {
        popScreen();
        return;
    }

    if (state.currentScreen == SCR_EQUALIZER) {
        showToast(EQ_PRESET_NAMES[eqPresetIdx]);
        popScreen();
        return;
    }

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
            case 4: loadDynamicFolder("queue", "Queue"); break;
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
            case 2: pushScreen(SCR_FLASHLIGHT, "flash", "Flashlight"); break;
            case 3: pushScreen(SCR_EQUALIZER, "eq", "Equalizer"); break;
            case 4: pushScreen(SCR_PC_STREAM, "pc", "PC Monitor"); break;
            case 5: brick.reset(); pushScreen(SCR_GAME_BRICK, "brick", "Brick"); break;
            case 6: snake.reset(); pushScreen(SCR_GAME_SNAKE, "snake", "Snake"); break;
            case 7: {
                const int opts[] = {0, 15, 30, 45, 60, 90};
                int next = 0;
                for (int i = 0; i < 6; i++) if (opts[i] == state.sleepMinutes) next = opts[(i + 1) % 6];
                state.sleepMinutes = next;
                state.sleepStartMs = millis();
                sendBleCommand("CMD:SLEEP:" + String(state.sleepMinutes));
                showToast(state.sleepMinutes == 0 ? "Sleep Timer: Off" : ("Sleep in " + String(state.sleepMinutes) + "m"));
                break;
            }
        }
        return;
    }

    if (state.currentScreen == SCR_MENU_SETTINGS) {
        switch (state.selectedIdx) {
            case 0: pushScreen(SCR_ABOUT, "about", "About"); break;
            case 1:
                state.uiStyle = (state.uiStyle == 1) ? 0 : 1;
                showToast(state.uiStyle == 1 ? "UI: Modern" : "UI: Classic");
                break;
            case 2: {
                const int timeouts[] = {5, 10, 30, 0};
                int cur = 0;
                for (int i = 0; i < 4; i++) if (timeouts[i] == state.sleepTimeoutSeconds) cur = i;
                state.sleepTimeoutSeconds = timeouts[(cur + 1) % 4];
                showToast(state.sleepTimeoutSeconds == 0 ? "Sleep: Never" : ("Sleep: " + String(state.sleepTimeoutSeconds) + "s"));
                break;
            }
            case 3: {
                state.brightnessLevel = (state.brightnessLevel + 1) % 4;
                const uint8_t contrasts[] = {25, 80, 160, 255};
                u8g2.setContrast(contrasts[state.brightnessLevel]);
                showToast(state.brightnessLevel == 0 ? "Bright: 25%" : (state.brightnessLevel == 1 ? "Bright: 50%" : (state.brightnessLevel == 2 ? "Bright: 75%" : "Bright: 100%")));
                break;
            }
            case 4:
                state.visualizerEnabled = !state.visualizerEnabled;
                showToast(state.visualizerEnabled ? "Visualizer: On" : "Visualizer: Off");
                break;
            case 5:
                state.shuffle = !state.shuffle;
                sendBleCommand("CMD:SHUFFLE:" + String(state.shuffle ? 1 : 0));
                showToast(state.shuffle ? "Shuffle: On" : "Shuffle: Off");
                break;
            case 6:
                state.repeatMode = (state.repeatMode + 1) % 3;
                sendBleCommand("CMD:REPEAT:" + String(state.repeatMode));
                showToast(state.repeatMode == 0 ? "Repeat: Off" : (state.repeatMode == 1 ? "Repeat: All" : "Repeat: One"));
                break;
            case 7:
                enterDeepSleep();
                break;
        }
        return;
    }

    if (state.currentScreen == SCR_MENU_DYNAMIC) {
        if (dynamicCount == 0) return;
        String chosen = dynamicItems[state.selectedIdx];
        int sel = state.selectedIdx;

        if (state.currentFolderId == "playlists") {
            loadDynamicFolder("pl_tracks:" + String(sel), chosen);
            return;
        } else if (state.currentFolderId == "artists") {
            loadDynamicFolder("art_tracks:" + String(sel), chosen);
            return;
        } else if (state.currentFolderId == "albums") {
            loadDynamicFolder("alb_tracks:" + String(sel), chosen);
            return;
        } else {
            sendBleCommand("PLAY_ITEM:" + state.currentFolderId + ":" + String(sel));
            state.trackTitle = chosen;
            state.positionSec = 0;
            state.isPlaying = true;
            pushScreen(SCR_NOW_PLAYING, "np", "Now Playing");
            return;
        }
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
    bool wakeIgnored = false;
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
                // Первое нажатие при спящем экране ТОЛЬКО включает экран, действие кнопки НЕ выполняется!
                isDisplaySleeping = false;
                u8g2.setPowerSave(0);
                lastActivityMs = now;
                btns[i].wakeIgnored = true;
                btns[i].lastLevel = lvl;
                continue;
            }
            btns[i].wakeIgnored = false;
            lastActivityMs = now;

            const char* bName = (btns[i].pin == PIN_BTN_UP) ? "UP" :
                                (btns[i].pin == PIN_BTN_DOWN) ? "DOWN" :
                                (btns[i].pin == PIN_BTN_LEFT) ? "LEFT" :
                                (btns[i].pin == PIN_BTN_RIGHT) ? "RIGHT" :
                                (btns[i].pin == PIN_BTN_CENTER) ? "CENTER" : "MENU";
            Serial.printf("BTN:%s:1\n", bName);
        } else if (lvl == LOW) {
            lastActivityMs = now;
            if (btns[i].wakeIgnored) {
                // Игнорируем удержание и повтор кнопки, которая разбудила экран
                btns[i].lastLevel = lvl;
                continue;
            }
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
            if (btns[i].wakeIgnored) {
                // Кнопка отпущена: сбрасываем флаг, действие кнопки НЕ вызывается!
                btns[i].wakeIgnored = false;
            } else if (now - btns[i].pressStartMs > 25 && !btns[i].longFired && !btns[i].repeatFired) {
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

void drawBluetoothIcon(int x, int y) {
    // Четкая пиксельная руна Bluetooth 7x9
    u8g2.drawVLine(x + 3, y, 9);
    u8g2.drawLine(x + 3, y, x + 6, y + 2);
    u8g2.drawLine(x + 6, y + 2, x + 1, y + 5);
    u8g2.drawLine(x + 1, y + 3, x + 6, y + 6);
    u8g2.drawLine(x + 6, y + 6, x + 3, y + 8);
}

void drawStatusBar() {
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);

    // Заголовок слева
    String title = state.currentMenuTitle;
    if (title.length() > 14) title = title.substring(0, 14);
    u8g2.drawUTF8(2, 9, title.c_str());

    // Иконка Play / Pause
    if (state.isPlaying) {
        u8g2.drawTriangle(107, 2, 107, 8, 112, 5);
    } else {
        u8g2.drawBox(107, 2, 2, 7);
        u8g2.drawBox(111, 2, 2, 7);
    }

    // Иконка Bluetooth (настоящая руна BT вместо буквы "B")
    if (state.bleConnected) {
        drawBluetoothIcon(118, 1);
    }

    // Разделительная линия статус-бара
    if (state.uiStyle == 1) {
        u8g2.drawHLine(2, 11, 124);
    } else {
        u8g2.drawHLine(0, 11, 128);
    }
}

String getRightValueForItem(int idx) {
    if (state.currentScreen == SCR_MENU_EXTRAS) {
        if (idx == 3) {
            return EQ_PRESET_NAMES[eqPresetIdx];
        }
        if (idx == 7) {
            return state.sleepMinutes == 0 ? "Off" : (String(state.sleepMinutes) + "m");
        }
    }
    if (state.currentScreen == SCR_MENU_SETTINGS) {
        switch (idx) {
            case 1: return state.uiStyle == 1 ? "Modern" : "Classic";
            case 2: return state.sleepTimeoutSeconds == 0 ? "Never" : (String(state.sleepTimeoutSeconds) + "s");
            case 3: return state.brightnessLevel == 0 ? "25%" : (state.brightnessLevel == 1 ? "50%" : (state.brightnessLevel == 2 ? "75%" : "100%"));
            case 4: return state.visualizerEnabled ? "On" : "Off";
            case 5: return state.shuffle ? "On" : "Off";
            case 6: return state.repeatMode == 0 ? "Off" : (state.repeatMode == 1 ? "All" : "One");
            case 7: return "";
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

        if (state.uiStyle == 1) {
            // Современное меню: скругленные аккуратные плашки (Pills)
            if (isSelected) {
                u8g2.setDrawColor(1);
                u8g2.drawRBox(1, yTop, total > visibleRows ? 119 : 126, 12, 3);
                u8g2.setDrawColor(0);
            } else {
                u8g2.setDrawColor(1);
            }

            String rightVal = getRightValueForItem(itemIdx);
            int maxLabelWidth = (total > visibleRows) ? 116 : 122;
            if (rightVal.length() > 0) {
                int rw = u8g2.getUTF8Width(rightVal.c_str());
                maxLabelWidth -= (rw + 4);
                u8g2.drawUTF8((total > visibleRows ? 116 : 122) - rw, yTop + 10, rightVal.c_str());
            }

            u8g2.setClipWindow(3, yTop, maxLabelWidth, yTop + 12);
            String label = getCurrentMenuItemLabel(itemIdx);
            u8g2.drawUTF8(4, yTop + 10, label.c_str());
            u8g2.setMaxClipWindow();
        } else {
            if (isSelected) {
                u8g2.setDrawColor(1);
                u8g2.drawBox(0, yTop, total > visibleRows ? 121 : 128, 12);
                u8g2.setDrawColor(0);
            } else {
                u8g2.setDrawColor(1);
            }

            String rightVal = getRightValueForItem(itemIdx);
            int maxLabelWidth = (total > visibleRows) ? 119 : 125;
            if (rightVal.length() > 0) {
                int rw = u8g2.getUTF8Width(rightVal.c_str());
                maxLabelWidth -= (rw + 4);
                u8g2.drawUTF8((total > visibleRows ? 119 : 125) - rw, yTop + 10, rightVal.c_str());
            }

            u8g2.setClipWindow(2, yTop, maxLabelWidth, yTop + 12);
            String label = getCurrentMenuItemLabel(itemIdx);
            u8g2.drawUTF8(3, yTop + 10, label.c_str());
            u8g2.setMaxClipWindow();
        }
    }

    // Скроллбар справа
    u8g2.setDrawColor(1);
    if (total > visibleRows) {
        if (state.uiStyle == 1) {
            int thumbH = max(6, (visibleRows * 48) / total);
            int maxTop = 48 - thumbH;
            int thumbY = 14 + (state.selectedIdx * maxTop) / max(1, total - 1);
            u8g2.drawRBox(124, thumbY, 3, thumbH, 1);
        } else {
            u8g2.drawFrame(123, 13, 5, 50);
            int thumbH = max(8, (visibleRows * 48) / total);
            int maxTop = 48 - thumbH;
            int thumbY = 14 + (state.selectedIdx * maxTop) / max(1, total - 1);
            u8g2.drawBox(124, thumbY, 3, thumbH);
        }
    }
}

void drawNowPlayingScreen() {
    u8g2.setDrawColor(1);

    if (state.uiStyle == 1) {
        // ================= СОВРЕМЕННЫЙ UI (SPLIT VIEW) =================
        // ЛЕВАЯ ПОЛОВИНА (x=1..51, y=13..63): ОБЛОЖКА АЛЬБОМА
        u8g2.drawRFrame(1, 13, 50, 50, 2);

        if (hasAlbumArt) {
            u8g2.drawXBM(2, 14, 48, 48, albumArtBitmap);
        } else {
            // Стильный винил при отсутствии обложки
            u8g2.drawCircle(26, 38, 20);
            u8g2.drawCircle(26, 38, 14);
            u8g2.drawCircle(26, 38, 8);
            u8g2.drawDisc(26, 38, 4);
            u8g2.setDrawColor(0);
            u8g2.drawDisc(26, 38, 1);
            u8g2.setDrawColor(1);
        }

        // ПРАВАЯ ПОЛОВИНА (x=54..127, y=13..63): ИНФОРМАЦИЯ О ТРЕКЕ
        if (state.showVolumeOverlay) {
            u8g2.drawRFrame(53, 16, 74, 44, 3);
            u8g2.setFont(u8g2_font_5x8_tf);
            u8g2.drawStr(60, 27, "VOLUME");
            char vBuf[8];
            snprintf(vBuf, sizeof(vBuf), "%d%%", state.volumePct);
            u8g2.drawStr(103, 27, vBuf);
            u8g2.drawRFrame(59, 33, 62, 6, 1);
            int vw = map(state.volumePct, 0, 100, 0, 58);
            if (vw > 0) u8g2.drawRBox(61, 35, vw, 2, 1);
            return;
        }

        // 1. Название трека с плавной бегущей строкой в пределах правого блока
        u8g2.setFont(u8g2_font_6x12_t_cyrillic);
        int titleW = u8g2.getUTF8Width(state.trackTitle.c_str());
        u8g2.setClipWindow(54, 12, 127, 26);
        if (titleW <= 73) {
            u8g2.drawUTF8(54, 24, state.trackTitle.c_str());
        } else {
            int scrollRange = titleW - 68;
            int offset = (millis() / 50) % (scrollRange + 25);
            int drawX = 54 - constrain(offset - 8, 0, scrollRange);
            u8g2.drawUTF8(drawX, 24, state.trackTitle.c_str());
        }
        u8g2.setMaxClipWindow();

        // 2. Исполнитель (с бегущей строкой)
        u8g2.setFont(u8g2_font_5x8_t_cyrillic);
        int artW = u8g2.getUTF8Width(state.trackArtist.c_str());
        u8g2.setClipWindow(54, 26, 127, 37);
        if (artW <= 73) {
            u8g2.drawUTF8(54, 35, state.trackArtist.c_str());
        } else {
            int scrollRange = artW - 68;
            int offset = (millis() / 55) % (scrollRange + 20);
            int drawX = 54 - constrain(offset - 6, 0, scrollRange);
            u8g2.drawUTF8(drawX, 35, state.trackArtist.c_str());
        }
        u8g2.setMaxClipWindow();

        // 3. Современный прогресс-бар
        int activePos = state.isScrubbing ? state.scrubPositionSec : state.positionSec;
        int pct = state.durationSec > 0 ? constrain((activePos * 100) / state.durationSec, 0, 100) : 0;
        int barW = 72;
        u8g2.drawRFrame(54, 41, barW, 4, 1);
        int pw = map(pct, 0, 100, 0, barW - 2);
        if (pw > 0) u8g2.drawRBox(55, 42, pw, 2, 1);
        if (state.isScrubbing) {
            u8g2.drawBox(54 + pw - 1, 40, 3, 6);
        }

        // 4. Время: текущее / общая длительность
        u8g2.setFont(u8g2_font_4x6_tf);
        String curT = formatSec(activePos);
        String totT = formatSec(state.durationSec);
        u8g2.drawStr(54, 52, curT.c_str());
        u8g2.drawStr(127 - u8g2.getStrWidth(totT.c_str()), 52, totT.c_str());

        // 5. Мета-строка внизу (номер в очереди + значки shuffle / repeat / like)
        String meta = "";
        if (state.isLiked) meta += "[L] ";
        if (state.shuffle) meta += "[S] ";
        if (state.repeatMode == 1) meta += "[RA] ";
        else if (state.repeatMode == 2) meta += "[R1] ";
        if (state.isScrubbing) meta = "[SEEK]";
        u8g2.drawStr(54, 62, meta.c_str());

        // Audio visualizer (Modern UI) with real frequencies from phone
        if (state.visualizerEnabled && state.isPlaying) {
            bool useReal = (millis() - state.lastSpecMs < 1500);
            uint32_t t = millis() / 90;
            for (int b = 0; b < 5; b++) {
                int h = useReal ? state.realSpec[b] : (1 + ((t * (b + 2) + b * 7) % 8));
                h = constrain(h, 1, 8);
                u8g2.drawVLine(106 + b * 4, 63 - h, h);
                u8g2.drawVLine(107 + b * 4, 63 - h, h);
            }
        }

    } else {
        // ================= КЛАССИЧЕСКИЙ IPOD UI =================
        u8g2.setFont(u8g2_font_5x8_tf);
        int leftX = 2;
        if (state.isLiked) {
            u8g2.drawGlyph(2, 21, 0x2665);
            u8g2.drawBox(2, 16, 5, 4);
            leftX = 10;
        }

        if (state.isScrubbing) {
            u8g2.drawStr(leftX, 21, "[Scrubbing]");
        } else {
            String modes = "";
            if (state.shuffle) modes += "SHF ";
            if (state.repeatMode == 1) modes += "R:ALL ";
            else if (state.repeatMode == 2) modes += "R:1 ";
            u8g2.drawStr(leftX, 21, modes.c_str());
        }

        // Audio visualizer (Classic UI) with real frequencies from phone
        if (state.visualizerEnabled && state.isPlaying) {
            bool useReal = (millis() - state.lastSpecMs < 1500);
            uint32_t t = millis() / 90;
            for (int b = 0; b < 5; b++) {
                int h = useReal ? state.realSpec[b] : (1 + ((t * (b + 3) + b * 5) % 8));
                h = constrain(h, 1, 8);
                u8g2.drawVLine(105 + b * 4, 21 - h, h);
                u8g2.drawVLine(106 + b * 4, 21 - h, h);
            }
        }

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

        u8g2.setFont(u8g2_font_5x8_t_cyrillic);
        String artist = state.trackArtist;
        if (artist.length() > 24) artist = artist.substring(0, 24);
        int artW = u8g2.getUTF8Width(artist.c_str());
        u8g2.drawUTF8(max(2, (128 - artW) / 2), 45, artist.c_str());

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
}

void drawFlashlightScreen() {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 128, 64);
}


// ======================= PC MONITOR STREAM ========================
uint8_t pcStreamBuffer[1024];

void processSerialPcStream() {
    static int rxState = 0;
    static int rxIdx = 0;

    while (Serial.available()) {
        uint8_t b = Serial.read();
        switch (rxState) {
            case 0:
                if (b == 0xAA) rxState = 1;
                break;
            case 1:
                if (b == 0x55) rxState = 2;
                else rxState = (b == 0xAA) ? 1 : 0;
                break;
            case 2:
                if (b == 0x01) {
                    rxState = 3;
                    rxIdx = 0;
                } else {
                    rxState = 0;
                }
                break;
            case 3:
                pcStreamBuffer[rxIdx++] = b;
                if (rxIdx >= 1024) rxState = 4;
                break;
            case 4:
                if (b == 0x55) rxState = 5;
                else rxState = 0;
                break;
            case 5:
                if (b == 0xAA) {
                    if (state.currentScreen != SCR_PC_STREAM) {
                        pushScreen(SCR_PC_STREAM, "pc", "PC Monitor");
                    }
                    lastActivityMs = millis();
                    memcpy(u8g2.getBufferPtr(), pcStreamBuffer, 1024);
                    u8g2.sendBuffer();
                }
                rxState = 0;
                break;
        }
    }
}

void drawPcStreamScreen() {
    memcpy(u8g2.getBufferPtr(), pcStreamBuffer, 1024);
}

void drawEqualizerScreen() {
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x12_tf);
    const char* pName = EQ_PRESET_NAMES[eqPresetIdx];
    int nw = u8g2.getStrWidth(pName);
    u8g2.drawStr((128 - nw) / 2, 23, pName);

    const char* freqLabels[] = {"60", "230", "910", "3.6k", "14k"};
    u8g2.setFont(u8g2_font_4x6_tf);
    for (int i = 0; i < 5; i++) {
        int x = 18 + i * 20;
        int dbVal = EQ_CURVES[eqPresetIdx][i];
        int barH = map(dbVal, -3, 7, 3, 24);
        barH = constrain(barH, 2, 24);

        u8g2.drawFrame(x, 52 - 24, 12, 25);
        u8g2.drawBox(x + 2, 52 - barH, 8, barH);

        int fw = u8g2.getStrWidth(freqLabels[i]);
        u8g2.drawStr(x + 6 - fw / 2, 62, freqLabels[i]);
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

    processSerialPcStream();
    pollButtons();

    uint32_t now = millis();

    // Автоотключение экрана через 10 секунд бездействия
    uint32_t activeTimeout = state.sleepTimeoutSeconds > 0 ? ((uint32_t)state.sleepTimeoutSeconds * 1000UL) : 0;
    if (!isDisplaySleeping && !isOtaUpdating && state.currentScreen != SCR_FLASHLIGHT && activeTimeout > 0 && (now - lastActivityMs >= activeTimeout)) {
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
        if (state.currentScreen != SCR_FLASHLIGHT && state.currentScreen != SCR_PC_STREAM) {
            drawStatusBar();
        }

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
            case SCR_FLASHLIGHT:
                drawFlashlightScreen();
                break;
            case SCR_EQUALIZER:
                drawEqualizerScreen();
                break;
            case SCR_PC_STREAM:
                drawPcStreamScreen();
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
