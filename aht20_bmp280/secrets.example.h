// Скопируй этот файл в secrets.h и впиши свои значения. secrets.h не попадает на GitHub (.gitignore).
// Copy this file to secrets.h and fill in your values. secrets.h is not committed (.gitignore).
//
// Ключи Groq / Gemini / Google Cloud и токен Telegram-бота сюда НЕ пишутся —
// они вводятся на странице колонки (⚙ Настройки) и хранятся в памяти платы.
// API keys and the Telegram bot token are entered on the device web page, not here.
#pragma once

// Wi-Fi для первого запуска (можно оставить пустым — тогда настройка через точку «Alexa-Setup»)
#define WIFI_SSID     "MyWiFi"
#define WIFI_PASSWORD "my-password"

// Пароль прошивки по Wi-Fi из Arduino IDE
#define OTA_PASS      "change-me-ota"

// Логин и пароль страницы /update и /factory
#define UPDATE_USER   "admin"
#define UPDATE_PASS   "change-me"
