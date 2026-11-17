# 🎵 iPodify: Spotify BLE Remote (ESP32-C3 Edition)

Физический пульт управления Spotify в дизайне классического плеера iPod на базе микроконтроллера **ESP32-C3 Super Mini**, **OLED-дисплея SSD1306 (128x64 I2C)** и 6 кнопок управления.

Устройство подключается к смартфону (Android / Web Bluetooth) по **Bluetooth Low Energy (BLE)** и транслирует в реальном времени:
- Название трека, исполнителя, альбом, текущую позицию и длительность.
- Управление воспроизведением (Play / Pause, Next, Prev, Volume Up / Down).
- Списки плейлистов и медиатеку Spotify прямо на экране плеера.
- Мгновенный отклик кнопок благодаря режиму `CONNECTION_PRIORITY_HIGH` (~15 мс задержка).

---

## 📸 Возможности

- **Экран Now Playing**: бегущая строка для длинных названий, индикатор батареи/громкости, тайминги, прогресс-бар.
- **Интерактивное меню iPod**: Музыка, Плейлисты, Артисты, Альбомы, Настройки.
- **Android Companion App**: фоновая служба (`Foreground Service`), автоподключение к пульту, синхронизация с системным плеером и Spotify Web API.
- **Web Bluetooth App (PWA)**: управление из любого Chromium-браузера без установки APK.
- **3D Корпус**: готовые STL файлы для печати на 3D принтере (корпус, крышка, кнопки).

---

## 🛠 Железо и распиновка

### Компоненты
1. **ESP32-C3 Super Mini**
2. **OLED дисплей 0.96" SSD1306 (128x64 I2C)**
3. **6 тактовых кнопок** (Крестовина / D-Pad + Центральная кнопка + Кнопка Menu)

### Распиновка (Pinout)

| Функция / Элемент | ESP32-C3 GPIO | Примечание |
|---|---|---|
| **OLED SDA** | **GPIO 6** | Шина I2C Data |
| **OLED SCL** | **GPIO 5** | Шина I2C Clock |
| **Кнопка ВЛЕВО (Prev)** | **GPIO 0** | Замыкание на GND (INPUT_PULLUP) |
| **Кнопка ВПРАВО (Next)** | **GPIO 1** | Замыкание на GND (INPUT_PULLUP) |
| **Кнопка ВНИЗ (Vol -)** | **GPIO 2** | Замыкание на GND (INPUT_PULLUP) |
| **Кнопка ЦЕНТР (Play / Select)** | **GPIO 3** | Замыкание на GND (INPUT_PULLUP) |
| **Кнопка ВВЕРХ (Vol +)** | **GPIO 4** | Замыкание на GND (INPUT_PULLUP) |
| **Кнопка MENU (Назад / Меню)** | **GPIO 7** | Замыкание на GND (INPUT_PULLUP) |
| **Питание дисплея** | **3.3V & GND** | От ESP32-C3 |

---

## 📱 Приложение-компаньон для телефона

### Вариант 1: Android Приложение (.APK) — Рекомендуется

Приложение работает в фоновом режиме, мгновенно передает нажатия кнопок и названия треков.

1. **Скачать APK**: файл [iPodify-Remote.apk](web/iPodify-Remote.apk) (или скачайте с GitHub Pages: `https://danmilk227.github.io/Spotify_remote-/iPodify-Remote.apk`).
2. Установите на телефон и выдайте разрешения:
   - Доступ к Bluetooth и поиск устройств поблизости.
   - **Доступ к уведомлениям / Media Listener** (чтобы приложение перехватывало треки Spotify).
3. **Авторизация Spotify (для подгрузки плейлистов)**:
   - В [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) откройте ваше приложение (Client ID: `49be876031464f1ba5a139eb691a4ef5`).
   - Нажмите **Settings** -> **Redirect URIs**.
   - Добавьте:
     - `ipodify://callback` (для Android приложения)
     - `https://danmilk227.github.io/Spotify_remote-/` (для Web версии)
   - Нажмите **Save**.
   - В приложении нажмите кнопку **"Авторизовать Spotify"** и подтвердите вход.

### Вариант 2: Веб-версия (Web Bluetooth)

- Откройте в Chrome на смартфоне: **https://danmilk227.github.io/Spotify_remote-/**
- Нажмите **"Подключить Bluetooth"**, выберите **iPodify-C3** и управляйте воспроизведением.

---

## 💻 Сборка и прошивка ESP32-C3

### Через PlatformIO (VS Code)
1. Откройте папку `firmware/` в VS Code с установленным расширением PlatformIO.
2. Подключите ESP32-C3 по Type-C.
3. Нажмите кнопку **Upload** (или `pio run -t upload`).

### Через Arduino IDE
1. Откройте файл `Arduino_IDE/iPodify_ESP32_C3/iPodify_ESP32_C3.ino`.
2. Выберите плату **ESP32C3 Dev Module** (Partition Scheme: Minimal SPIFFS или Huge App).
3. Установите библиотеки: `U8g2`, `ESP32 BLE Arduino`.
4. Нажмите **Загрузить**.

---

## 🖨️ 3D Корпус (Case)

Все 3D-модели расположены в папке [`case/`](case/):
- `iPodify_Front_Shell.stl` — Лицевая панель с окном под экран и кнопками.
- `iPodify_Back_Cover.stl` — Задняя крышка корпуса.
- `iPodify_4_Buttons.stl` — Блок стрелок (D-Pad).
- `iPodify_Center_Button.stl` — Центральная кнопка.
- `iPodify_Wheel.stl` — Декоративное кольцо в стиле iPod ClickWheel.
- Исходники параметрических моделей в формате OpenSCAD (`.scad`) и FreeCAD (`.FCStd`, `.FCMacro`).

---

## 📂 Структура репозитория

```
├── .github/workflows/       # GitHub Actions для автоматической сборки прошивки и APK
├── android/                 # Исходный код Android приложения (Kotlin)
├── Arduino_IDE/             # Проект для компиляции через Arduino IDE (.ino)
├── case/                    # 3D модели корпуса и кнопок (STL, CAD, FreeCAD)
├── firmware/                # Исходный код прошивки PlatformIO (C++)
│   ├── src/main.cpp         # Логика прошивки, UI и обработка BLE
│   └── platformio.ini       # Конфигурация PlatformIO
├── web/                     # Web Bluetooth PWA и хостинг APK для GitHub Pages
└── README.md                # Документация проекта
```
