// Умная колонка на ESP32-S3 N16R8: голосовой ассистент «Alexa», радио, будильники, датчики AHT20 + BMP280
// Подключение датчика: VDD -> 3V3, SDA -> GPIO8, GND -> GND, SCL -> GPIO9
// Подключение MAX98357A: VIN -> 5V, GND -> GND, BCLK -> GPIO4, LRC -> GPIO5, DIN -> GPIO6, SD -> 3V3, GAIN — не подключать
// Полная схема: docs/wiring.png
// Подключение INMP441: VDD -> 3V3, GND -> GND, SCK -> GPIO15, WS -> GPIO16, SD -> GPIO17, L/R -> GND
//
// Библиотеки (менеджер библиотек Arduino IDE):
//   - "Adafruit AHTX0"
//   - "Adafruit BMP280 Library"
//   - "ESP32-audioI2S" от schreibfaul1 (если нет в менеджере — ZIP с github.com/schreibfaul1/ESP32-audioI2S,
//     Скетч -> Подключить библиотеку -> Добавить .ZIP библиотеку)
//   - "ArduinoJson" от Benoit Blanchon
//
// Настройки платы: ESP32S3 Dev Module, Flash Size 16MB, PSRAM OPI PSRAM,
//   USB CDC On Boot: Enabled,
//   Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)" — но реально берётся partitions.csv из папки скетча
//   (там есть раздел model для слова «Alexa»). Модель прошивается отдельно — см. README_wakeword.txt
//
// Веб-страница: http://meteo.local (или по IP из Serial Monitor)
// Прошивка по Wi-Fi (OTA): Инструменты -> Порт -> сетевой порт "meteo", пароль: OTA_PASS ниже
// Прошивка через сайт: http://meteo.local/update (логин/пароль ниже),
//   файл .bin: Скетч -> Экспорт скомпилированного бинарного файла,
//   он появится в папке скетча: build/esp32.esp32.esp32s3/aht20_bmp280.ino.bin

#include "weblog.h"  // первым: журнал Serial ещё и на странице /log
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <mbedtls/platform.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include "audio_player.h"
#include "mic.h"
#include "assistant.h"
#include "wakeword.h"
#include "gemini_live.h"
#include "web_pages.h"

// Свои пароли — в secrets.h (скопируй secrets.example.h). Файла нет — берутся значения ниже,
// а сеть Wi-Fi настраивается с телефона через точку «Alexa-Setup» (wifi_setup.h).
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID     ""           // пусто — сразу режим настройки Wi-Fi
#define WIFI_PASSWORD ""
#endif
#ifndef OTA_PASS
#define OTA_PASS      "alexa-ota"  // пароль для прошивки по Wi-Fi из Arduino IDE
#endif
#ifndef UPDATE_USER
#define UPDATE_USER   "admin"      // логин/пароль для страницы прошивки и сброса
#define UPDATE_PASS   "alexa"
#endif
#define HOST_NAME     "meteo"      // адрес страницы будет http://meteo.local

#include "wifi_setup.h"
#include "telegram.h"

#define I2C_SDA 8
#define I2C_SCL 9

// Встроенный RGB-светодиод. На большинстве плат ESP32-S3 это GPIO48,
// на некоторых — GPIO38. Если не горит — поменяй тут.
#define RGB_PIN        48
#define LED_BRIGHTNESS 20          // 0..255, слабее — меньше слепит ночью

#define READ_INTERVAL_MS    5000    // как часто читать датчики
#define HISTORY_INTERVAL_S  60      // точка на графике раз в минуту
#define HISTORY_DAYS        14      // сколько дней хранить историю (в PSRAM)
#define TREND_MINUTES       180     // прогноз по изменению давления за 3 часа

Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp(&Wire);
bool ahtOk = false;
bool bmpOk = false;

WebServer web(80);
bool webStarted = false;

Preferences prefs;
bool ledEnabled = true;

// Последние прочитанные значения
float curTemp = 20.0;
float curHum = 50.0;
float curPressure = 1013.0;  // гПа

// ---------- История измерений (кольцевой буфер в PSRAM) ----------

struct Sample {
  uint32_t t;      // секунды с момента включения
  int16_t temp;    // °C * 100
  uint16_t hum;    // % * 10
  uint16_t press;  // гПа * 10
};

Sample *hist = nullptr;
uint32_t histCap = 0;
uint32_t histCount = 0;
uint32_t histHead = 0;  // куда писать следующую точку

// Суммы для усреднения за минуту
float sumT = 0, sumH = 0, sumP = 0;
uint32_t sumN = 0;

uint32_t uptimeSec() {
  return esp_timer_get_time() / 1000000ULL;  // millis() переполняется через 49 дней, это нет
}

// i = 0 — самая старая точка
Sample &histAt(uint32_t i) {
  return hist[(histHead + histCap - histCount + i) % histCap];
}

void setupHistory() {
  histCap = HISTORY_DAYS * 24 * 3600 / HISTORY_INTERVAL_S;
  hist = (Sample *)ps_malloc(histCap * sizeof(Sample));
  if (!hist) {
    // PSRAM не включена — храним хотя бы сутки в обычной памяти
    histCap = 24 * 3600 / HISTORY_INTERVAL_S;
    hist = (Sample *)malloc(histCap * sizeof(Sample));
  }
  Serial.printf("История: %lu точек (%.1f дн.)\n", (unsigned long)histCap, histCap * HISTORY_INTERVAL_S / 86400.0);
}

void recordHistory() {
  static uint32_t lastRecord = 0;
  uint32_t now = uptimeSec();
  if (now - lastRecord < HISTORY_INTERVAL_S || sumN == 0 || !hist) return;
  lastRecord = now;

  Sample s;
  s.t = now;
  s.temp = (int16_t)lroundf(sumT / sumN * 100);
  s.hum = (uint16_t)lroundf(sumH / sumN * 10);
  s.press = (uint16_t)lroundf(sumP / sumN * 10);
  sumT = sumH = sumP = 0;
  sumN = 0;

  hist[histHead] = s;
  histHead = (histHead + 1) % histCap;
  if (histCount < histCap) histCount++;
}

// ---------- Прогноз по давлению ----------

// Изменение давления за последние 3 часа, гПа. false — пока мало данных.
bool pressureTrend(float &dp) {
  if (!bmpOk || histCount <= TREND_MINUTES) return false;
  dp = ((int)histAt(histCount - 1).press - (int)histAt(histCount - 1 - TREND_MINUTES).press) / 10.0;
  return true;
}

const char *forecastText(bool ok, float dp) {
  if (!bmpOk) return "Нет данных с датчика давления";
  if (!ok) return "Копим данные — прогноз появится через 3 часа после включения";
  if (dp <= -6.0) return "Давление резко падает — сильный ветер, гроза, резкое ухудшение";
  if (dp <= -1.5) return "Давление падает — погода портится, возможны осадки";
  if (dp >= 6.0) return "Давление резко растёт — ветрено, быстрое прояснение";
  if (dp >= 1.5) return "Давление растёт — погода улучшается";
  return "Давление стабильно — погода без изменений";
}

// ---------- Датчики ----------

uint8_t readChipId(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0xD0);
  if (Wire.endTransmission() != 0) return 0;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

void setupSensors() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  ahtOk = aht.begin(&Wire);
  Serial.println(ahtOk ? "AHT20 OK" : "AHT20 не найден :(");

  const uint8_t addrs[] = {0x77, 0x76};
  for (uint8_t addr : addrs) {
    uint8_t id = readChipId(addr);
    if (id != 0 && bmp.begin(addr, id)) {
      bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                      Adafruit_BMP280::SAMPLING_X2,
                      Adafruit_BMP280::SAMPLING_X16,
                      Adafruit_BMP280::FILTER_X16,
                      Adafruit_BMP280::STANDBY_MS_500);
      bmpOk = true;
      break;
    }
  }
  Serial.println(bmpOk ? "BMP280 OK" : "BMP280 не найден :(");
}

void readSensors() {
  static uint32_t lastRead = 0;
  if (lastRead != 0 && millis() - lastRead < READ_INTERVAL_MS) return;
  lastRead = millis();

  if (ahtOk) {
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    curTemp = temp.temperature;
    curHum = humidity.relative_humidity;
  }
  if (bmpOk) {
    curPressure = bmp.readPressure() / 100.0F;
    if (!ahtOk) curTemp = bmp.readTemperature();
  }

  sumT += curTemp;
  sumH += curHum;
  sumP += curPressure;
  sumN++;
}

// ---------- RGB-светодиод ----------
//   мигает красным  — нет Wi-Fi
//   зелёный         — комфортно
//   синий / оранжевый — холодно (<18 °C) / жарко (>27 °C)
//   голубой / фиолетовый — сухо (<35 %) / сыро (>65 %)

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  static int32_t lastColor = -1;
  int32_t color = (r << 16) | (g << 8) | b;
  if (color == lastColor) return;
  lastColor = color;
  rgbLedWrite(RGB_PIN, r, g, b);
}

void updateLed() {
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  const uint8_t B = LED_BRIGHTNESS;

  if (wifiSetupActive) {  // режим настройки Wi-Fi — медленно мигает синим
    setLed(0, 0, (millis() / 1000) % 2 ? B : 0);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    bool on = (millis() / 500) % 2;
    setLed(on ? B : 0, 0, 0);
    return;
  }
  // белый — колонка слушает: запись с микрофона или живой разговор с Gemini (там записи нет, звук идёт сразу)
  if (micRecording || asState == AS_LISTENING) {
    setLed(B, B, B);
    return;
  }
  if (asState == AS_THINKING) {  // мигает голубым — ассистент думает
    bool on = (millis() / 250) % 2;
    setLed(0, on ? B : 0, on ? B : 0);
    return;
  }
  if (!ledEnabled || nightActive()) setLed(0, 0, 0);  // ночью не светим
  else if (curTemp < 18)  setLed(0, 0, B);
  else if (curTemp > 27)  setLed(B, B / 3, 0);
  else if (curHum < 35)   setLed(0, B, B);
  else if (curHum > 65)   setLed(B, 0, B);
  else                    setLed(0, B, 0);
}

// ---------- Веб-страница ----------

void handleRoot() {
  web.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleNow() {
  float dp = 0;
  bool ok = pressureTrend(dp);

  JsonDocument doc;
  doc["t"] = curTemp;
  doc["h"] = curHum;
  doc["p"] = curPressure;
  if (ok) doc["dp"] = dp;
  else doc["dp"] = nullptr;
  doc["fc"] = forecastText(ok, dp);
  doc["led"] = ledEnabled ? 1 : 0;
  doc["night"] = nightActive() ? formatClock(nightUntil, false) : String("");  // ночной режим — до скольки
  doc["sleep"] = sleepStopAt ? formatClock(sleepStopAt, false) : String("");   // таймер сна
  doc["air"] = airStatus();
  doc["airv"] = airVoice ? 1 : 0;
  doc["rssi"] = WiFi.RSSI();
  doc["ssid"] = wifiSsid;
  doc["up"] = uptimeSec();
  doc["vol"] = volume;
  doc["voice"] = ttsVoice;
  doc["ttsStatus"] = ttsStatus;
  doc["gcKey"] = gcloudKey.length() < 8 ? String("") : String("…") + gcloudKey.substring(gcloudKey.length() - 4);
  doc["gcChars"] = gcloudMonthChars;
  doc["gcLimit"] = GCLOUD_MONTH_LIMIT;
  doc["vvol"] = voiceVolume;
  doc["vboost"] = voiceBoostDb;
  doc["play"] = audioPlaying ? 1 : 0;
  doc["station"] = radioUrl[0] ? radioName : "";  // название станции и трека — в них бывают кавычки,
  doc["title"] = radioUrl[0] ? nowTitle : "";     // поэтому JSON собираем через ArduinoJson
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

// Отдаёт историю за последние h часов, не больше ~400 точек (лишние прореживаются)
void handleHistory() {
  uint32_t hours = web.hasArg("h") ? web.arg("h").toInt() : 24;
  if (hours == 0) hours = 24;
  uint32_t now = uptimeSec();
  uint32_t from = now > hours * 3600 ? now - hours * 3600 : 0;

  uint32_t first = histCount;
  while (first > 0 && histAt(first - 1).t >= from) first--;
  uint32_t n = histCount - first;
  uint32_t step = n > 400 ? (n + 399) / 400 : 1;

  String json;
  json.reserve(n / step * 32 + 64);
  json += "{\"up\":";
  json += now;
  json += ",\"d\":[";
  bool firstItem = true;
  for (uint32_t i = first; i < histCount; i += step) {
    const Sample &s = histAt(i);
    if (!firstItem) json += ',';
    firstItem = false;
    json += '[';
    json += s.t;    json += ',';
    json += s.temp; json += ',';
    json += s.hum;  json += ',';
    json += s.press;
    json += ']';
  }
  json += "]}";
  web.send(200, "application/json", json);
}

// /api/night?on=1|0 — ночной режим; /api/air?voice=1|0 — предупреждать голосом о воздухе
void handleNight() {
  if (web.arg("on") == "1") nightStart();
  else nightEnd();
  web.send(200, "application/json", "{}");
}

void handleAir() {
  if (web.hasArg("voice")) {
    airVoice = web.arg("voice") == "1";
    prefs.putBool("airv", airVoice);
  }
  web.send(200, "application/json", "{}");
}

void handleLed() {
  if (web.hasArg("on")) {
    ledEnabled = web.arg("on") == "1";
    prefs.putBool("led", ledEnabled);
  }
  web.send(200, "application/json", ledEnabled ? "{\"led\":1}" : "{\"led\":0}");
}

// /api/audio?a=play&url=...  |  a=say&text=...  |  a=stop  |  a=vol&v=0..21
// /api/mic?a=rec&s=5 — начать запись, /api/mic?gain=4&noise=30 — настройки,
// /api/mic — состояние и уровень
void handleMic() {
  if (web.arg("a") == "rec") micStartRecording(web.hasArg("s") ? web.arg("s").toInt() : 5, true);
  if (web.arg("a") == "recafe") wakeStartRecording(web.hasArg("s") ? web.arg("s").toInt() : 5);  // что слышит WakeNet
  if (web.hasArg("gain")) {
    micGain = constrain(web.arg("gain").toFloat(), 1.0f, 32.0f);
    prefs.putFloat("mgain", micGain);
  }
  if (web.hasArg("noise")) {
    micNoise = constrain(web.arg("noise").toInt(), 0, 100);
    prefs.putInt("mnoise", micNoise);
  }
  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":%d,\"rec\":%d,\"level\":%d,\"sec\":%.1f,\"gain\":%.1f,\"noise\":%d}",
           micBuf ? 1 : 0, (micRecording && micTestRecording) ? 1 : 0, micLevel, micSamples / (float)MIC_RATE,
           (float)micGain, (int)micNoise);
  web.send(200, "application/json", json);
}

// Последняя запись в формате WAV — браузер проигрывает её сам
void handleMicWav() {
  if (micRecording || !micBuf) {
    web.send(409, "text/plain", "busy");
    return;
  }
  uint32_t dataBytes = micSamples * 2;
  uint8_t header[44];
  micWavHeader(header, dataBytes);

  web.setContentLength(sizeof(header) + dataBytes);
  web.send(200, "audio/wav", "");
  web.sendContent((const char *)header, sizeof(header));
  const char *data = (const char *)micBuf;
  for (uint32_t off = 0; off < dataBytes; off += 4096) {
    web.sendContent(data + off, min((uint32_t)4096, dataBytes - off));
  }
}

// /api/assistant?a=talk — начать/закончить фразу, a=ask&q=... — вопрос текстом,
// /api/assistant — состояние
void handleAssistant() {
  String a = web.arg("a");
  if (a == "talk") assistantTrigger();
  else if (a == "stop") assistantDismiss();
  else if (a == "ask") assistantAsk(web.arg("q"));
  if (web.hasArg("wake")) {  // включить/выключить слово «Alexa»
    wwEnabled = web.arg("wake") == "1";
    prefs.putBool("wake", wwEnabled);
  }
  if (web.hasArg("thr")) {  // чувствительность слова «Alexa»
    wakeSetThreshold(web.arg("thr").toFloat());
    prefs.putFloat("wakethr", wwThreshold);
  }
  if (web.hasArg("aec")) {  // включить/выключить эхоподавление для слова «Alexa»
    wakeSetAec(web.arg("aec") == "1");
    prefs.putBool("aec", wwAec);
  }

  static const char *states[] = {"idle", "listening", "thinking"};
  JsonDocument doc;
  doc["state"] = states[asState];
  doc["heard"] = asHeard;
  doc["answer"] = asAnswer;
  doc["error"] = asError;
  doc["model"] = asModel;
  doc["wake"] = wwEnabled ? 1 : 0;
  doc["aec"] = wwAec ? 1 : 0;
  doc["thr"] = wwThreshold;
  doc["live"] = liveEnabled ? 1 : 0;
  doc["liveStatus"] = liveStatus;
  doc["liveVoice"] = liveVoice;
  doc["liveModel"] = liveModel;
  doc["liveKey"] = liveKey.length() < 8 ? String("") : String("…") + liveKey.substring(liveKey.length() - 4);
  doc["wakeStatus"] = wwStatus;
  doc["wakeCount"] = (uint32_t)wwDetections;
  // ключ целиком не отдаём — только последние 4 символа, чтобы было видно, что он сохранён
  doc["key"] = groqKey.length() < 8 ? String("") : String("…") + groqKey.substring(groqKey.length() - 4);
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

// POST /api/gemini — живой разговор: key=..., model=..., voice=..., on=0/1 (любые из них)
void handleGeminiLive() {
  if (web.hasArg("key")) {
    liveKey = web.arg("key");
    liveKey.trim();
    prefs.putString("gmkey", liveKey);
  }
  if (web.hasArg("model")) {
    liveModel = web.arg("model");
    liveModel.trim();
    if (liveModel.isEmpty()) liveModel = LIVE_MODEL_DEFAULT;
    prefs.putString("gmmodel", liveModel);
  }
  if (web.hasArg("voice")) {
    liveVoice = web.arg("voice");
    prefs.putString("gmvoice", liveVoice);
  }
  if (web.hasArg("on")) {
    liveEnabled = web.arg("on") == "1";
    prefs.putBool("live", liveEnabled);
  }
  if (liveKey.isEmpty()) strlcpy(liveStatus, "ключ не задан", sizeof(liveStatus));
  else snprintf(liveStatus, sizeof(liveStatus), "готов (%s, голос %s)", liveModel.c_str(), liveVoice.c_str());
  web.send(200, "application/json", "{\"ok\":1}");
}

// /api/spotify: GET — состояние и ссылка для входа; POST id=..., secret=..., code=<адрес из браузера>, device=...
void handleSpotify() {
  if (web.method() == HTTP_POST) {
    if (web.hasArg("id")) {
      spClientId = web.arg("id");
      spClientId.trim();
      prefs.putString("spid", spClientId);
    }
    if (web.hasArg("secret")) {
      spClientSecret = web.arg("secret");
      spClientSecret.trim();
      prefs.putString("spsec", spClientSecret);
    }
    if (web.hasArg("id") || web.hasArg("secret")) {  // новое приложение — старый вход недействителен
      spRefresh = "";
      spAccess = "";
      prefs.remove("sprefresh");
      strlcpy(spStatus, "нажми «Войти в Spotify»", sizeof(spStatus));
    }
    if (web.hasArg("device")) {
      spDevice = web.arg("device");
      spDevice.trim();
      prefs.putString("spdev", spDevice);
    }
    if (web.hasArg("stream")) {
      spStreamUrl = web.arg("stream");
      spStreamUrl.trim();
      prefs.putString("spstream", spStreamUrl);
      if (!spStreamUrl.isEmpty() && spDevice.isEmpty()) spDevice = "Kolonka";
    }
    if (web.hasArg("code")) spotifyLoginWithCode(web.arg("code"));
    if (web.hasArg("logout")) {
      spRefresh = "";
      spAccess = "";
      prefs.remove("sprefresh");
      strlcpy(spStatus, "вышли из Spotify", sizeof(spStatus));
    }
  }
  JsonDocument doc;
  doc["status"] = spStatus;
  doc["ready"] = spotifyReady();
  doc["hasApp"] = !spClientId.isEmpty() && !spClientSecret.isEmpty();
  doc["id"] = spClientId;  // Client ID не секрет; Secret наружу не отдаём
  doc["device"] = spDevice;
  doc["stream"] = spStreamUrl;
  doc["redirect"] = SPOTIFY_REDIRECT;
  if (!spClientId.isEmpty()) doc["authUrl"] = spotifyAuthUrl();
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

// POST /api/gcloud, key=... — ключ Google Cloud Text-to-Speech для красивого голоса
// /api/telegram — состояние бота; POST token=... — токен от @BotFather; POST del=id — отвязать чат
void handleTelegram() {
  if (web.hasArg("token")) tgSetToken(web.arg("token"));
  if (web.hasArg("del")) tgUnlink(strtoll(web.arg("del").c_str(), nullptr, 10));
  JsonDocument doc;
  doc["status"] = tgStatus;
  doc["has"] = !tgToken.isEmpty();
  doc["code"] = tgCode;
  tgLock();
  JsonArray list = doc["chats"].to<JsonArray>();
  for (JsonObject c : tgChats.as<JsonArray>()) {
    JsonObject o = list.add<JsonObject>();
    o["id"] = String(c["id"].as<int64_t>());  // строкой: в JavaScript такие большие числа теряют точность
    o["n"] = c["n"];
  }
  tgUnlock();
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

void handleGcloudKey() {
  gcloudKey = web.arg("key");
  gcloudKey.trim();
  prefs.putString("gckey", gcloudKey);
  web.send(200, "application/json", "{\"ok\":1}");
}

// POST /api/groq, key=... и/или model=... — сохранить API-ключ Groq и модель
void handleGroqKey() {
  if (web.hasArg("key")) {
    groqKey = web.arg("key");
    groqKey.trim();
    prefs.putString("groq", groqKey);
  }
  if (web.hasArg("model")) {
    asModel = web.arg("model");
    asModel.trim();
    if (asModel.isEmpty()) asModel = GROQ_CHAT_MODEL;
    prefs.putString("llm", asModel);
  }
  web.send(200, "application/json", "{\"ok\":1}");
}

// Список станций для страницы
void handleStations() {
  JsonDocument doc;
  for (size_t i = 0; i < STATION_COUNT; i++) {
    JsonObject s = doc.add<JsonObject>();
    s["name"] = STATIONS[i].name;
    s["url"] = STATIONS[i].url;
  }
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

// /api/events — таймеры, будильники и напоминания; /api/events?del=3 — удалить
void handleEvents() {
  if (web.hasArg("del")) eventCancel(web.arg("del").toInt());
  time_t now = time(nullptr);
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const Event &e : events) {
    if (!e.id) continue;
    JsonObject o = list.add<JsonObject>();
    o["id"] = e.id;
    o["text"] = eventDescribe(e, now);
  }
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

// /api/radio?q=jazz — поиск в каталоге: сначала по названию станции, потом по жанру
void handleRadioSearch() {
  String q = web.arg("q");
  q.trim();
  if (q.length() && radioSearch("", q, "", "") == 0) radioSearch(q, "", "", "");

  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const FoundStation &s : radioFound) {
    JsonObject o = list.add<JsonObject>();
    o["name"] = s.name;
    o["url"] = s.url;
  }
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

void handleAudio() {
  String a = web.arg("a");
  if (a == "play" && web.hasArg("url")) {
    radioPlay(web.arg("url"), web.arg("name"));
  } else if (a == "say" && web.hasArg("text")) {
    sendAudioCmd(AUDIO_SAY, web.arg("text"));
  } else if (a == "stop") {
    radioStop();
  } else if (a == "vvol" && web.hasArg("v")) {  // громкость голоса, 0..21 — отдельно от музыки
    audioSetVoiceVolume(web.arg("v").toInt());
    prefs.putUChar("vvol", voiceVolume);
  } else if (a == "vboost" && web.hasArg("v")) {  // усиление голоса на плате, -6..+12 дБ
    voiceBoostDb = constrain(web.arg("v").toFloat(), -6.0f, 12.0f);
    prefs.putFloat("vboost", voiceBoostDb);
  } else if (a == "voice") {  // голос: ru-RU-Chirp3-HD-Leda и т. п., пусто — простой Google Translate
    strlcpy(ttsVoice, web.arg("v").c_str(), sizeof(ttsVoice));
    prefs.putString("voice", ttsVoice);
  } else if (a == "vol" && web.hasArg("v")) {
    audioSetVolume(web.arg("v").toInt());
    prefs.putUChar("vol", volume);
  } else {
    web.send(400, "text/plain", "bad request");
    return;
  }
  web.send(200, "application/json", "{\"ok\":1}");
}

// ---------- Прошивка через сайт ----------

bool updateAllowed = false;

void handleUpdatePage() {
  if (!web.authenticate(UPDATE_USER, UPDATE_PASS)) return web.requestAuthentication();
  web.send_P(200, "text/html; charset=utf-8", UPDATE_HTML);
}

// Вызывается по кусочкам, пока файл загружается
void handleUpdateUpload() {
  HTTPUpload &up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    updateAllowed = web.authenticate(UPDATE_USER, UPDATE_PASS);
    if (!updateAllowed) return;
    Serial.printf("Прошивка через сайт: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (!updateAllowed) {
    return;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    setLed((millis() / 200) % 2 ? LED_BRIGHTNESS : 0, (millis() / 200) % 2 ? LED_BRIGHTNESS : 0, 0);  // мигает жёлтым
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("Прошивка записана: %u байт\n", up.totalSize);
    else Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

// Сброс к заводским — перед подарком: стираются сеть Wi-Fi, все ключи, вход в Spotify, будильники и настройки.
// Колонка перезагружается в режим настройки Wi-Fi. Защищён тем же паролем, что и прошивка.
void handleFactory() {
  if (!web.authenticate(UPDATE_USER, UPDATE_PASS)) return web.requestAuthentication();
  if (web.method() != HTTP_POST) {
    web.send_P(200, "text/html; charset=utf-8", FACTORY_HTML);
    return;
  }
  Serial.println("Сброс к заводским настройкам");
  // Telegram-бота можно оставить — чтобы и дальше обслуживать колонку издалека
  bool keepTg = web.arg("keeptg") == "1";
  String tgTok = prefs.getString("tgtoken", ""), tgList = prefs.getString("tgchats", "[]");
  prefs.clear();
  prefs.putBool("wmig", true);  // сеть из кода обратно не подставлять — новый хозяин настроит свою
  if (keepTg) {
    prefs.putString("tgtoken", tgTok);
    prefs.putString("tgchats", tgList);
  }
  web.send(200, "text/plain; charset=utf-8",
           "Готово. Колонка перезагружается и включит сеть " SETUP_AP_NAME " для настройки Wi-Fi.");
  delay(1500);
  ESP.restart();
}

// Вызывается после загрузки файла
void handleUpdateDone() {
  if (!updateAllowed) return web.requestAuthentication();
  if (Update.hasError() || !Update.isFinished()) {
    web.send(500, "text/plain; charset=utf-8", String("не удалось записать прошивку: ") + Update.errorString());
    return;
  }
  web.send(200, "text/plain; charset=utf-8", "OK");
  delay(500);
  ESP.restart();
}

void updateWeb() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!webStarted) {
    // Часы по интернету — нужны ассистенту, чтобы знать время и дату (часовой пояс Киева)
    configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");

    // Прошивка по Wi-Fi из Arduino IDE; заодно запускает имя meteo.local
    ArduinoOTA.setHostname(HOST_NAME);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([] { sendAudioCmd(AUDIO_STOP); });  // чтобы звук не мешал прошивке
    ArduinoOTA.begin();
    MDNS.addService("http", "tcp", 80);

    web.on("/", handleRoot);
    web.on("/api/stations", handleStations);
    web.on("/api/radio", handleRadioSearch);
    web.on("/api/events", handleEvents);
    web.on("/api/now", handleNow);
    web.on("/api/history", handleHistory);
    web.on("/api/led", handleLed);
    web.on("/api/night", handleNight);
    web.on("/api/air", handleAir);
    web.on("/api/audio", handleAudio);
    web.on("/api/mic", handleMic);
    web.on("/mic.wav", handleMicWav);
    web.on("/api/assistant", handleAssistant);
    web.on("/api/groq", HTTP_POST, handleGroqKey);
    web.on("/api/gcloud", HTTP_POST, handleGcloudKey);
    web.on("/api/gemini", HTTP_POST, handleGeminiLive);
    web.on("/api/spotify", handleSpotify);
    web.on("/api/telegram", handleTelegram);
    web.on("/log", [] { web.send(200, "text/plain; charset=utf-8", webLog.text()); });
    web.on("/wifi/save", HTTP_POST, handleWifiSave);
    web.on("/factory", handleFactory);
    web.on("/update", HTTP_GET, handleUpdatePage);
    web.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    web.begin();
    webStarted = true;
    Serial.printf("Веб-страница: http://%s.local или http://%s\n", HOST_NAME, WiFi.localIP().toString().c_str());
    Serial.printf("Память: %s\n", heapInfo().c_str());
  }
  web.handleClient();
  ArduinoOTA.handle();
}

// ---------- Wi-Fi ----------

// Защищённые соединения (HTTPS) по умолчанию берут ~45 КБ внутренней памяти на каждое, а её мало и она
// дробится — соединения с Groq и Google перестают открываться. Отдаём шифрованию память из PSRAM (8 МБ);
// если там вдруг не хватит — берём внутреннюю, как раньше.
void *tlsCalloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void tlsFree(void *p) {
  heap_caps_free(p);
}

void setup() {
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);  // до любых соединений
  Serial.begin(115200);
  delay(1000);

  prefs.begin("meteo");
  ledEnabled = prefs.getBool("led", true);
  airVoice = prefs.getBool("airv", true);
  volume = prefs.getUChar("vol", volume);
  strlcpy(ttsVoice, prefs.getString("voice", ttsVoice).c_str(), sizeof(ttsVoice));
  if (ttsVoice[0] && strncmp(ttsVoice, "ru-RU-Chirp3", 12) != 0)  // старый выбор голоса Microsoft — сбрасываем на Leda
    strlcpy(ttsVoice, TTS_DEFAULT_VOICE, sizeof(ttsVoice));
  gcloudKey = prefs.getString("gckey", "");
  gcloudLoadUsage();
  voiceVolume = prefs.getUChar("vvol", voiceVolume);
  voiceBoostDb = prefs.getFloat("vboost", voiceBoostDb);

  micGain = prefs.getFloat("mgain", micGain);
  micNoise = prefs.getInt("mnoise", micNoise);

  groqKey = prefs.getString("groq", "");
  asModel = prefs.getString("llm", GROQ_CHAT_MODEL);

  setupAudio();
  setupMic();
  eventsLoad();
  setupAssistant();
  wwEnabled = prefs.getBool("wake", true);
  wwAec = prefs.getBool("aec", true);
  wwThreshold = prefs.getFloat("wakethr", wwThreshold);
  setupWakeWord();
  liveKey = prefs.getString("gmkey", "");
  liveModel = prefs.getString("gmmodel", LIVE_MODEL_DEFAULT);
  liveVoice = prefs.getString("gmvoice", liveVoice);
  liveEnabled = prefs.getBool("live", true);
  setupLive();
  spotifyLoad();
  setupTelegram();
  setupHistory();
  setupSensors();
  readSensors();
  setupWiFi();
}

void loop() {
  checkWiFi();
  readSensors();
  recordHistory();
  updateLed();
  updateWeb();
  checkBootButton();
  checkEvents();
  checkAir();
  delay(1);  // небольшая пауза, чтобы цикл не крутился впустую и не мешал другим задачам
}
