// Живой разговор с Gemini Live API: одна модель сама слушает, думает, ищет в интернете и отвечает голосом.
// Вместо цепочки «запись → распознавание → ответ текстом → синтез речи» — одно соединение (WebSocket):
//   плата шлёт звук с микрофона (16 кГц, уже после эхоподавления) и сразу получает звук ответа (24 кГц),
//   который подмешивается в выходной поток аудиобиблиотеки (audio_player.h) — почти без задержки.
// Команды колонки (радио, таймеры, громкость…) Gemini вызывает как функции — выполняет их asRunAction().
// Если ключа нет или соединение не получилось — ассистент работает по-старому (Groq + Google TTS).
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include "audio_player.h"
#include "mic.h"
#include "assistant.h"

#define LIVE_HOST "generativelanguage.googleapis.com"
#define LIVE_PATH "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
#define LIVE_MODEL_DEFAULT "gemini-3.8-live"

#define LIVE_FIRST_WAIT_MS 8000           // если за столько после «Alexa» никто не заговорил — заканчиваем
#define LIVE_IDLE_MS       8000           // после ответа столько ждём продолжения разговора
#define LIVE_MAX_MS        (9 * 60000UL)  // соединение у Google живёт ~10 минут — закрываем чуть раньше
#define LIVE_SEND_SAMPLES  1600           // шлём звук кусками по 100 мс
#define LIVE_UP_RING       64000          // 4 с звука с микрофона (16 кГц)
#define LIVE_DOWN_RING     (24000 * 30)   // 30 с ответа (24 кГц), в PSRAM

String liveKey;                         // ключ из AI Studio (бесплатный проект)
bool liveEnabled = true;                // можно выключить на странице — тогда работает старая схема
String liveVoice = "Kore";              // голос ответа: Kore, Leda, Aoede, Charon, Puck…
String liveModel = LIVE_MODEL_DEFAULT;
// Встроенный поиск Google в разговоре. На бесплатном тарифе его квота нулевая (Gemini закрывает
// разговор с «exceeded your current quota»), поэтому по умолчанию выключен — вместо него функция
// web_search через Groq. Если включить, а квоты нет, при первом же отказе выключится сам.
bool liveUseSearch = false;
volatile uint32_t liveToolDoneAt = 0;
char liveStatus[160] = "ключ не задан";

// ---------- Звук с микрофона (для Gemini) ----------

int16_t *liveUp = nullptr;
volatile uint32_t liveUpW = 0;
uint32_t liveUpR = 0;
volatile bool liveCapture = false;
// Пока колонка говорит (и ещё немного после — эхо в комнате), вместо микрофона шлём тишину.
// Эхоподавление не справляется с громким динамиком: Gemini слышал сам себя и отвечал сам себе.
// Перебить ответ голосом при этом нельзя — только кнопкой BOOT.
#define LIVE_ECHO_TAIL_MS 700
volatile uint32_t liveMuteUntil = 0;

void liveUplink(const int16_t *s, int n) {
  if (!liveCapture || !liveUp) return;
  bool mute = (int32_t)(liveMuteUntil - millis()) > 0;
  for (int i = 0; i < n; i++) liveUp[(liveUpW + i) % LIVE_UP_RING] = mute ? 0 : s[i];
  liveUpW += n;
}

// ---------- Звук ответа ----------

int16_t *liveDown = nullptr;
volatile uint32_t liveDownW = 0;  // пишет задача ассистента
volatile uint32_t liveDownR = 0;  // читает аудиозадача
volatile bool liveFlush = false;  // перебили — выбросить то, что ещё не прозвучало

// Звук от Gemini приходит пачками с неровными паузами. Если играть сразу, очередь то и дело пустеет
// и речь «спотыкается». Поэтому копим запас и начинаем (или продолжаем после опустения) только когда
// набралось LIVE_PREBUF отсчётов либо новые данные перестали приходить — значит, это хвост ответа.
#define LIVE_PREBUF       (24000 * 3 / 2)  // 1,5 с: Gemini присылает ~15–22 тыс. отсчётов/с при норме 24 тыс.
#define LIVE_TAIL_WAIT_MS 600  // столько тишины от Gemini — значит, ответ кончился, доигрываем остаток
volatile uint32_t liveLastAudioAt = 0;
volatile bool livePlaying = false;
volatile uint32_t liveOutSamples = 0;  // сколько отсчётов 48 кГц ушло на динамик (норма 48000/с)
volatile uint32_t liveUnderruns = 0;   // сколько раз очередь опустела посреди речи

bool liveModelSpeaking() {
  return liveDownR != liveDownW;
}

// Подставляет голос ответа вместо «тишины» прямо перед выводом: 24 кГц → 48 кГц, моно → оба канала
void livePlayback(int32_t *buff, int16_t n) {
  static int16_t prev = 0, cur = 0;
  static bool half = false;
  if (liveFlush) {
    liveDownR = liveDownW;
    liveFlush = false;
    livePlaying = false;
    prev = cur = 0;
  }
  if (!livePlaying) {
    uint32_t queued = liveDownW - liveDownR;
    if (queued >= LIVE_PREBUF || (queued && millis() - liveLastAudioAt > LIVE_TAIL_WAIT_MS)) livePlaying = true;
  }
  liveOutSamples += n / 2;
  for (int i = 0; i + 1 < n; i += 2) {
    int32_t v;
    if (!half) {
      prev = cur;
      if (livePlaying && liveDownR != liveDownW) {
        cur = liveDown[liveDownR % LIVE_DOWN_RING];
        liveDownR = liveDownR + 1;
        liveMuteUntil = millis() + LIVE_ECHO_TAIL_MS;
      } else {
        if (livePlaying) {  // очередь кончилась — снова копим запас
          livePlaying = false;
          if (millis() - liveLastAudioAt < LIVE_TAIL_WAIT_MS) liveUnderruns++;  // данные ещё шли — не успели
        }
        cur = 0;
      }
      v = ((int32_t)prev + cur) / 2;  // промежуточный отсчёт — среднее соседних
    } else {
      v = cur;
    }
    half = !half;
    buff[i] = buff[i + 1] = v << 16;  // библиотека держит отсчёты в 32-битном масштабе
  }
}

void livePushAudio(const int16_t *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    // Буфер полон (ответ идёт быстрее, чем звучит) — немного ждём, пока проиграется
    while (liveDownW - liveDownR >= LIVE_DOWN_RING - 1 && !liveFlush) vTaskDelay(pdMS_TO_TICKS(10));
    liveDown[liveDownW % LIVE_DOWN_RING] = s[i];
    liveDownW = liveDownW + 1;
  }
  liveLastAudioAt = millis();
}

// ---------- Минимальный клиент WebSocket ----------

WiFiClientSecure liveWs;

bool liveReadExact(uint8_t *buf, size_t n, uint32_t deadline) {
  size_t got = 0;
  while (got < n) {
    if (millis() > deadline || (!liveWs.connected() && !liveWs.available())) return false;
    int a = liveWs.available();
    if (a <= 0) {
      vTaskDelay(1);
      continue;
    }
    int r = liveWs.read(buf + got, min((size_t)a, n - got));
    if (r > 0) got += r;
  }
  return true;
}

// Сообщение клиента (по стандарту клиент «маскирует» данные)
bool liveWsSend(const char *data, size_t len, uint8_t opcode = 0x1) {
  uint8_t head[14];
  size_t h = 0;
  head[h++] = 0x80 | opcode;
  if (len < 126) {
    head[h++] = 0x80 | len;
  } else if (len < 65536) {
    head[h++] = 0x80 | 126;
    head[h++] = len >> 8;
    head[h++] = len & 0xFF;
  } else {
    head[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) head[h++] = (uint64_t)len >> (8 * i);
  }
  uint8_t m[4];
  uint32_t mask = esp_random();
  memcpy(m, &mask, 4);
  memcpy(head + h, m, 4);
  h += 4;

  uint8_t *payload = (uint8_t *)ps_malloc(len ? len : 1);
  if (!payload) return false;
  for (size_t i = 0; i < len; i++) payload[i] = data[i] ^ m[i % 4];
  bool ok = liveWs.write(head, h) == h;
  for (size_t off = 0; ok && off < len;) {
    size_t n = liveWs.write(payload + off, min((size_t)4096, len - off));
    if (!n) ok = false;
    off += n;
  }
  free(payload);
  return ok;
}

bool liveWsSend(const String &s) {
  return liveWsSend(s.c_str(), s.length());
}

// Читает одно целое сообщение сервера (склеивает части). Данные — в PSRAM, освободить free().
// false — сообщений пока нет (или соединение закрылось — смотри liveWs.connected()).
bool liveWsRead(uint8_t *&msg, size_t &msgLen) {
  msg = nullptr;
  msgLen = 0;
  if (liveWs.available() < 2) return false;
  uint32_t deadline = millis() + 5000;
  for (;;) {
    uint8_t b[2];
    if (!liveReadExact(b, 2, deadline)) break;
    bool fin = b[0] & 0x80;
    uint8_t op = b[0] & 0x0F;
    uint64_t n = b[1] & 0x7F;
    if (n == 126) {
      uint8_t e[2];
      if (!liveReadExact(e, 2, deadline)) break;
      n = (e[0] << 8) | e[1];
    } else if (n == 127) {
      uint8_t e[8];
      if (!liveReadExact(e, 8, deadline)) break;
      n = 0;
      for (int i = 0; i < 8; i++) n = (n << 8) | e[i];
    }
    if (n > 4 * 1024 * 1024) break;
    uint8_t *part = (uint8_t *)ps_malloc(n + 1);
    if (!part || !liveReadExact(part, n, deadline)) {
      free(part);
      break;
    }
    part[n] = 0;

    if (op == 0x9) {  // ping — отвечаем pong
      liveWsSend((const char *)part, n, 0xA);
      free(part);
      if (!msg) return false;
      continue;
    }
    if (op == 0x8) {  // сервер закрыл соединение: 2 байта кода и причина
      unsigned code = n >= 2 ? (part[0] << 8) | part[1] : 0;
      snprintf(liveStatus, sizeof(liveStatus), "Gemini закрыл соединение (%u): %s", code, n > 2 ? (char *)part + 2 : "");
      Serial.printf("[live] %s\n", liveStatus);
      free(part);
      liveWs.stop();
      free(msg);
      msg = nullptr;
      return false;
    }
    if (op == 0xA) {  // pong
      free(part);
      if (!msg) return false;
      continue;
    }
    // Текст, двоичное или продолжение — склеиваем
    uint8_t *joined = (uint8_t *)ps_realloc(msg, msgLen + n + 1);
    if (!joined) {
      free(part);
      break;
    }
    msg = joined;
    memcpy(msg + msgLen, part, n);
    msgLen += n;
    msg[msgLen] = 0;
    free(part);
    if (fin) return true;
  }
  free(msg);
  msg = nullptr;
  liveWs.stop();
  return false;
}

bool liveConnect() {
  liveWs.setInsecure();
  liveWs.setTimeout(10000);
  if (!liveWs.connect(LIVE_HOST, 443)) {
    strlcpy(liveStatus, "нет связи с Google", sizeof(liveStatus));
    return false;
  }
  uint8_t keyRaw[16];
  for (auto &k : keyRaw) k = esp_random();
  unsigned char wsKey[32];
  size_t wsKeyLen = 0;
  mbedtls_base64_encode(wsKey, sizeof(wsKey), &wsKeyLen, keyRaw, sizeof(keyRaw));
  wsKey[wsKeyLen] = 0;

  // Ключ передаём заголовком x-goog-api-key, как и в других запросах к Google (Cloud TTS).
  // С ?key= в адресе новый ключ (AQ.…) получал «quota exceeded» при нулевых счётчиках проекта.
  liveWs.print(String("GET ") + LIVE_PATH + " HTTP/1.1\r\n"
               "Host: " LIVE_HOST "\r\n"
               "x-goog-api-key: " + liveKey + "\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Key: " + String((char *)wsKey) + "\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n");

  String status = liveWs.readStringUntil('\n');
  if (status.indexOf(" 101") < 0) {
    snprintf(liveStatus, sizeof(liveStatus), "Google не принял соединение: %s", status.c_str());
    Serial.printf("[live] %s\n", liveStatus);
    liveWs.stop();
    return false;
  }
  for (;;) {  // пропускаем остальные заголовки ответа
    String line = liveWs.readStringUntil('\n');
    if (line == "\r" || line.isEmpty()) break;
  }
  return true;
}

// ---------- Настройка разговора ----------

JsonObject liveFn(JsonArray fns, const char *name, const char *desc) {
  JsonObject f = fns.add<JsonObject>();
  f["name"] = name;
  f["description"] = desc;
  return f;
}

void liveParam(JsonObject f, const char *name, const char *type, const char *desc, bool required = true) {
  JsonObject p = f["parameters"];
  if (p.isNull()) {
    p = f["parameters"].to<JsonObject>();
    p["type"] = "OBJECT";
  }
  JsonObject props = p["properties"];
  if (props.isNull()) props = p["properties"].to<JsonObject>();
  JsonObject x = props[name].to<JsonObject>();
  x["type"] = type;
  x["description"] = desc;
  if (required) {
    JsonArray r = p["required"];
    if (r.isNull()) r = p["required"].to<JsonArray>();
    r.add(name);
  }
}

String liveSystemPrompt() {
  String s =
    "Ты — голосовой помощник по имени Ассистент, как Алиса или Siri. Ты живёшь в умной колонке в квартире "
    "в Киеве. Тебя зовут словом «Алекса» — если услышишь его в начале фразы, это просто обращение к тебе. "
    "Всегда говори только по-русски, даже если фраза расслышана плохо или похожа на другой язык — "
    "тогда переспроси по-русски. Говори тепло, естественно и разборчиво, в обычном темпе. Обычно отвечай коротко, 1–3 "
    "предложения, но если нужен подробный ответ (рецепт, инструкция, сказка для ребёнка, объяснение) — "
    "отвечай полностью. Ты универсальный помощник: отвечай на любые вопросы. "
    "Погоду в Киеве бери из данных ниже. "
    "Управляй колонкой только через функции: радио, громкость музыки и твоего голоса, светодиод, таймеры, "
    "будильники (можно с музыкой), напоминания, ночной режим и таймер сна. Выполняя команду, коротко подтверди её голосом. Никогда не говори, что сделала "
    "что-то (включила, выключила, поставила), если не вызвала для этого функцию — сначала вызови функцию. "
    "Просят просто музыку без станции — вызови radio_play со станцией подходящего жанра.Конкретную песню включить нельзя — "
    "предложи станцию подходящего жанра. На «доброе утро» — утренняя сводка: день недели, дата, время, погода "
    "на сегодня, температура в комнате. Если просят то, чего у тебя нет (свет, телевизор), честно скажи.\n\n";
  if (spotifyReady())
    s += "Подключён Spotify: конкретную песню, исполнителя, альбом или плейлист включай функцией spotify_play — "
         "это главнее правила «конкретную песню включить нельзя». Радиофункции — только когда просят радио или "
         "станцию. После spotify_play скажи, что именно включила (это будет в ответе функции).\n\n";
  if (liveUseSearch)
    s += "Для свежей и меняющейся информации (новости, курсы валют, цены, спорт, события) пользуйся поиском Google.\n\n";
  else if (!groqKey.isEmpty())
    s += "Для свежей и меняющейся информации (новости, курсы валют, цены, спорт, события, всё, в чём не уверена) "
         "вызывай функцию web_search и перескажи найденное своими словами. Поиск идёт несколько секунд — "
         "перед вызовом коротко скажи «Секунду, посмотрю». Не выдумывай свежие факты без поиска.\n\n";
  else
    s += "Доступа к интернету у тебя сейчас нет: про свежие новости, курсы и цены честно скажи, что не знаешь.\n\n";
  s += asContextText();
  return s;
}

bool liveSendSetup() {
  JsonDocument doc;
  JsonObject setup = doc["setup"].to<JsonObject>();
  setup["model"] = "models/" + liveModel;
  JsonObject gen = setup["generationConfig"].to<JsonObject>();
  gen["responseModalities"].to<JsonArray>().add("AUDIO");
  gen["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = liveVoice;
  gen["speechConfig"]["languageCode"] = "ru-RU";  // без этого обрывок фразы может принять за испанский
  setup["systemInstruction"]["parts"][0]["text"] = liveSystemPrompt();

  JsonArray tools = setup["tools"].to<JsonArray>();
  if (liveUseSearch) tools.add<JsonObject>()["googleSearch"].to<JsonObject>();
  JsonArray fns = tools.add<JsonObject>()["functionDeclarations"].to<JsonArray>();

  String ids;
  for (size_t i = 0; i < STATION_COUNT; i++) ids += String(i ? ", " : "") + STATIONS[i].id + " (" + STATIONS[i].name + ")";
  static String stationDesc;
  stationDesc = "id любимой станции: " + ids + ". На просто «включи радио» — hitfm";

  JsonObject f = liveFn(fns, "radio_play", "Включить одну из любимых радиостанций (заиграет после разговора)");
  liveParam(f, "station", "STRING", stationDesc.c_str());
  f = liveFn(fns, "radio_search", "Найти и включить станцию в мировом каталоге радио по жанру, стране, языку или названию");
  liveParam(f, "tag", "STRING", "жанр или настроение одним словом по-английски: jazz, rock, lofi, classical…", false);
  liveParam(f, "name", "STRING", "часть названия станции", false);
  liveParam(f, "countrycode", "STRING", "двухбуквенный код страны: UA, PL, US…", false);
  liveParam(f, "language", "STRING", "язык по-английски: ukrainian, english…", false);
  liveFn(fns, "radio_next", "Включить следующую станцию из последнего поиска");
  liveFn(fns, "radio_stop", "Выключить радио");
  f = liveFn(fns, "volume", "Громкость музыки 0–21; на «тише» или «громче» меняй на 3");
  liveParam(f, "value", "INTEGER", "громкость 0–21");
  f = liveFn(fns, "voice_volume", "Громкость твоего голоса 0–21; на «говори громче или тише» меняй на 3");
  liveParam(f, "value", "INTEGER", "громкость 0–21");
  f = liveFn(fns, "led", "Включить или выключить светодиод на колонке");
  liveParam(f, "on", "BOOLEAN", "true — включить");
  f = liveFn(fns, "timer", "Поставить таймер");
  liveParam(f, "seconds", "INTEGER", "на сколько секунд");
  liveParam(f, "label", "STRING", "необязательная подпись, например «паста»", false);
  f = liveFn(fns, "alarm", "Поставить будильник");
  liveParam(f, "time", "STRING", "время ЧЧ:ММ");
  liveParam(f, "repeat", "STRING", "once, daily или weekdays", false);
  liveParam(f, "station", "STRING", "если просят разбудить музыкой или радио — id любимой станции (просто «с музыкой» — hitfm)", false);
  if (spotifyReady()) {
    liveParam(f, "spotify", "STRING", "разбудить музыкой из Spotify: что включить (вместо station)", false);
    liveParam(f, "spotify_type", "STRING", "track, artist, album или playlist", false);
  }
  f = liveFn(fns, "good_night", "«Спокойной ночи», «я спать»: ночной режим до 7 утра — светодиод гаснет, голос тише, "
                                "без предупреждений; музыка выключается. Пожелай спокойной ночи коротко");
  liveParam(f, "minutes", "INTEGER", "сколько ещё играть музыке, если просят «поиграй ещё полчаса»; иначе 0", false);
  f = liveFn(fns, "sleep_timer", "Выключить музыку через столько минут; 0 — отменить таймер сна");
  liveParam(f, "minutes", "INTEGER", "через сколько минут");
  liveFn(fns, "good_morning", "Выключить ночной режим («мы проснулись», «выключи ночной режим»)");
  f = liveFn(fns, "reminder", "Напоминание на дату и время (считай от текущего времени)");
  liveParam(f, "at", "STRING", "ГГГГ-ММ-ДД ЧЧ:ММ");
  liveParam(f, "text", "STRING", "о чём напомнить, коротко");
  f = liveFn(fns, "cancel", "Отменить таймер, будильник или напоминание по номеру из списка");
  liveParam(f, "id", "INTEGER", "номер");
  f = liveFn(fns, "cancel_all", "Отменить все таймеры, будильники или напоминания");
  liveParam(f, "type", "STRING", "timer, alarm, reminder или all");
  if (spotifyReady()) {
    f = liveFn(fns, "spotify_play", "Найти и включить в Spotify песню, исполнителя, альбом или плейлист. "
                                    "Играет на устройстве пользователя, где открыт Spotify (телефон, компьютер)");
    liveParam(f, "query", "STRING", "что искать: название и исполнитель, как сказали («Обійми Океан Ельзи»)");
    liveParam(f, "type", "STRING", "track, artist, album или playlist; на «включи такого-то исполнителя» — artist");
    f = liveFn(fns, "spotify_control", "Управление Spotify: пауза, продолжить, следующий или предыдущий трек");
    liveParam(f, "action", "STRING", "pause, resume, next или previous");
    liveFn(fns, "spotify_now", "Узнать, что сейчас играет в Spotify");
  }
  if (!liveUseSearch && !groqKey.isEmpty()) {
    f = liveFn(fns, "web_search", "Найти свежую информацию в интернете: новости, курсы валют, цены, спорт, "
                                  "события, расписания, всё, что могло измениться. Вернёт короткую сводку");
    liveParam(f, "query", "STRING", "поисковый запрос");
    liveParam(f, "question", "STRING", "вопрос пользователя целиком, как он его задал", false);
  }

  JsonObject vad = setup["realtimeInputConfig"]["automaticActivityDetection"].to<JsonObject>();
  vad["silenceDurationMs"] = 700;  // пауза, после которой Gemini считает, что ты договорил
  vad["prefixPaddingMs"] = 200;
  setup["inputAudioTranscription"].to<JsonObject>();   // текст того, что сказал ты, — для страницы
  setup["outputAudioTranscription"].to<JsonObject>();  // и того, что ответила она

  String body;
  serializeJson(doc, body);
  return liveWsSend(body);
}

// ---------- Разбор сообщений ----------

bool liveSetupDone = false;
bool liveTurnDone = false;      // Gemini договорила ответ
bool liveNewTurn = true;        // следующий текст распознавания — уже новая реплика
bool liveEndAfterTurn = false;  // включили музыку — закончим разговор после ответа
bool liveGoAway = false;
uint32_t liveFirstAudioAt = 0;

void liveAppend(char *dst, size_t cap, const char *text) {
  size_t len = strlen(dst);
  if (len + 1 < cap) strlcpy(dst + len, text, cap - len);
}

void liveHandleToolCall(JsonObject toolCall) {
  JsonDocument resp;
  JsonArray out = resp["toolResponse"]["functionResponses"].to<JsonArray>();
  for (JsonObject fc : toolCall["functionCalls"].as<JsonArray>()) {
    if (fc["name"] == "web_search") {
      // Поиск в интернете делает Groq (у Gemini на бесплатном тарифе свой поиск недоступен),
      // а найденное Gemini пересказывает своим голосом
      String query = fc["args"]["query"] | "";
      String question = fc["args"]["question"] | query.c_str();
      Serial.printf("[live] ищу в интернете: %s (%s)\n", query.c_str(), heapInfo().c_str());
      String found = asWebSearch(query, question);
      groqClose();  // соединение с Groq больше не нужно — освобождаем память
      JsonObject r = out.add<JsonObject>();
      r["id"] = fc["id"];
      r["name"] = fc["name"];
      r["response"]["result"] = found;
      liveToolDoneAt = millis();  // поиск мог идти долго — не считаем это паузой в разговоре
      continue;
    }
    if (fc["name"] == "spotify_play") {
      // Сам запрос к Spotify — после разговора (см. spPendingQuery), сейчас только подтверждаем
      spPendingQuery = fc["args"]["query"] | "";
      spPendingType = fc["args"]["type"] | "track";
      liveEndAfterTurn = true;
      Serial.printf("[live] включу в Spotify после ответа: %s (%s)\n", spPendingQuery.c_str(), spPendingType.c_str());
      JsonObject r = out.add<JsonObject>();
      r["id"] = fc["id"];
      r["name"] = fc["name"];
      r["response"]["result"] = "Принято: сразу после твоего ответа включу в Spotify «" + spPendingQuery +
                                "». Скажи одной короткой фразой, что включаешь.";
      liveToolDoneAt = millis();
      continue;
    }
    JsonDocument action;
    action.set(fc["args"]);
    action["do"] = fc["name"];
    asMusicChanged = false;
    asActionInfo = "";
    String failed = asRunAction(action.as<JsonObject>());
    if (asMusicChanged || action["do"] == "good_night") liveEndAfterTurn = true;
    JsonObject r = out.add<JsonObject>();
    r["id"] = fc["id"];
    r["name"] = fc["name"];
    // Успех — «ok» или что именно сделали (например, какую песню нашли в Spotify)
    r["response"]["result"] = !failed.isEmpty() ? failed : !asActionInfo.isEmpty() ? asActionInfo : String("ok");
    liveToolDoneAt = millis();
  }
  String body;
  serializeJson(resp, body);
  liveWsSend(body);
}

void liveHandleMessage(const uint8_t *msg, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, (const char *)msg, len)) return;

  if (doc["setupComplete"].is<JsonObject>()) liveSetupDone = true;
  if (doc["goAway"].is<JsonObject>()) liveGoAway = true;
  if (doc["toolCall"].is<JsonObject>()) liveHandleToolCall(doc["toolCall"]);

  JsonObject sc = doc["serverContent"];
  if (sc.isNull()) return;

  if (sc["interrupted"] | false) {  // перебили голосом — замолкаем
    liveFlush = true;
    Serial.println("[live] перебили");
  }
  const char *heard = sc["inputTranscription"]["text"] | (const char *)nullptr;
  if (heard) {
    if (liveNewTurn) {
      asHeard[0] = 0;
      asAnswer[0] = 0;
      liveNewTurn = false;
    }
    liveAppend(asHeard, sizeof(asHeard), heard);
  }
  const char *said = sc["outputTranscription"]["text"] | (const char *)nullptr;
  if (said) liveAppend(asAnswer, sizeof(asAnswer), said);

  for (JsonObject part : sc["modelTurn"]["parts"].as<JsonArray>()) {
    const char *b64 = part["inlineData"]["data"] | (const char *)nullptr;
    if (!b64) continue;
    size_t b64Len = strlen(b64), cap = b64Len * 3 / 4 + 4, n = 0;
    uint8_t *pcm = (uint8_t *)ps_malloc(cap);
    if (pcm && mbedtls_base64_decode(pcm, cap, &n, (const unsigned char *)b64, b64Len) == 0) {
      if (!liveFirstAudioAt) liveFirstAudioAt = millis();
      livePushAudio((const int16_t *)pcm, n / 2);
    }
    free(pcm);
  }
  if (sc["turnComplete"] | false) {
    liveTurnDone = true;
    liveNewTurn = true;
    Serial.printf("[live] Вы: %s\n[live] Ассистент: %s\n", asHeard, asAnswer);
  }
}

// Отправляем накопленный звук с микрофона кусками по 100 мс
bool liveSendAudio() {
  static char *msg = nullptr;
  static const size_t b64Cap = LIVE_SEND_SAMPLES * 2 * 4 / 3 + 8;
  if (!msg) msg = (char *)ps_malloc(b64Cap + 128);
  if (liveUpW - liveUpR > LIVE_UP_RING) liveUpR = liveUpW - LIVE_UP_RING;  // слишком отстали — догоняем
  // Пока колонка говорит, микрофон всё равно заглушён — не шлём тишину вовсе: защищённое соединение
  // меньше занято отправкой и быстрее принимает звук ответа
  if ((int32_t)(liveMuteUntil - millis()) > 0) {
    liveUpR = liveUpW;
    return true;
  }
  while (liveUpW - liveUpR >= LIVE_SEND_SAMPLES) {
    int16_t block[LIVE_SEND_SAMPLES];
    for (int i = 0; i < LIVE_SEND_SAMPLES; i++) block[i] = liveUp[(liveUpR + i) % LIVE_UP_RING];
    liveUpR += LIVE_SEND_SAMPLES;

    static const char pre[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
    size_t p = strlen(pre), n = 0;
    memcpy(msg, pre, p);
    mbedtls_base64_encode((unsigned char *)msg + p, b64Cap, &n, (const unsigned char *)block, sizeof(block));
    p += n;
    memcpy(msg + p, "\"}}}", 4);
    p += 4;
    if (!liveWsSend(msg, p)) return false;
  }
  return true;
}

// ---------- Разговор ----------

void liveSetBuffers() {
  if (!liveUp) liveUp = (int16_t *)ps_malloc(LIVE_UP_RING * sizeof(int16_t));
  if (!liveDown) liveDown = (int16_t *)ps_malloc(LIVE_DOWN_RING * sizeof(int16_t));
}

void liveFinish() {
  liveCapture = false;
  liveWs.stop();
  sendAudioCmd(AUDIO_STOP);  // выключить «тишину» и вернуть громкость музыки
  asState = AS_IDLE;
}

// Вызывается задачей ассистента после «Alexa» или кнопки. false — живой разговор не получился.
bool liveSession() {
  if (!liveEnabled || liveKey.isEmpty() || WiFi.status() != WL_CONNECTED) return false;
  liveSetBuffers();
  if (!liveUp || !liveDown) {
    strlcpy(liveStatus, "не хватило памяти", sizeof(liveStatus));
    return false;
  }

  asState = AS_LISTENING;
  // Звук с микрофона начинаем копить сразу — пока подключаемся, первые слова не потеряются
  liveUpR = liveUpW;
  liveCapture = true;
  liveFlush = true;
  sendAudioCmd(AUDIO_LIVE);

  liveSetupDone = liveTurnDone = liveEndAfterTurn = liveGoAway = false;
  liveNewTurn = true;
  liveFirstAudioAt = 0;
  asHeard[0] = 0;
  asAnswer[0] = 0;

  uint32_t t0 = millis();
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !liveSetupDone; attempt++) {
    t0 = millis();
    Serial.printf("[live] подключаюсь%s, %s\n", liveUseSearch ? "" : " (без поиска Google)", heapInfo().c_str());
    ok = liveConnect();
    if (ok && !liveSendSetup()) {
      snprintf(liveStatus, sizeof(liveStatus), "не удалось отправить настройку (%s)", heapInfo().c_str());
      ok = false;
    }
    while (ok && !liveSetupDone && millis() - t0 < 6000) {
      uint8_t *m;
      size_t n;
      if (liveWsRead(m, n)) {
        liveHandleMessage(m, n);
        free(m);
      } else if (!liveWs.connected()) {
        ok = false;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    // На бесплатном тарифе у поиска Google своя квота, и бывает, что она нулевая — тогда Gemini
    // закрывает разговор с «exceeded your current quota». Пробуем ещё раз без поиска и запоминаем.
    if (!liveSetupDone && liveUseSearch && strstr(liveStatus, "quota")) {
      liveUseSearch = false;
      liveWs.stop();
      continue;
    }
    break;
  }
  if (!ok || !liveSetupDone) {
    if (ok) strlcpy(liveStatus, "Gemini не ответил на настройку", sizeof(liveStatus));
    else if (!liveStatus[0] || !strncmp(liveStatus, "готов", strlen("готов")))
      snprintf(liveStatus, sizeof(liveStatus), "соединение оборвалось (%s)", heapInfo().c_str());
    Serial.printf("[live] не получилось (%s) — работаю по-старому\n", liveStatus);
    liveFinish();
    return false;
  }
  snprintf(liveStatus, sizeof(liveStatus), "работает (%s, голос %s)", liveModel.c_str(), liveVoice.c_str());
  Serial.printf("[live] подключился за %lu мс\n", (unsigned long)(millis() - t0));

  bool userSpoke = false;
  uint32_t lastActivity = millis();
  uint32_t statAt = millis(), statR = liveDownR, statW = liveDownW, statUp = liveUpR, statOut = liveOutSamples;
  liveUnderruns = 0;
  for (;;) {
    if (!liveSendAudio()) break;

    // Раз в 5 с — проверка скорости: проигрывать должно ~24000 отсчётов в секунду, иначе голос
    // звучит замедленно (меньше) или ускоренно (больше)
    if (millis() - statAt >= 5000) {
      float sec = (millis() - statAt) / 1000.0f;
      uint32_t played = liveDownR - statR, got = liveDownW - statW, sent = liveUpR - statUp;
      uint32_t out = liveOutSamples - statOut;
      if (played || got)
        Serial.printf("[live] звук: проиграно %.0f/с (норма 24000), получено %.0f/с, в очереди %.1f с, "
                      "отправлено с микрофона %.0f/с (норма 16000), на динамик %.0f/с (норма 48000), обрывов %u\n",
                      played / sec, got / sec, (liveDownW - liveDownR) / 24000.0f, sent / sec, out / sec,
                      (unsigned)liveUnderruns);
      statAt = millis();
      statR = liveDownR;
      statW = liveDownW;
      statUp = liveUpR;
      statOut = liveOutSamples;
      liveUnderruns = 0;
    }

    for (int k = 0; k < 16; k++) {  // разбираем всё, что пришло, но не застреваем надолго
      uint8_t *m;
      size_t n;
      if (!liveWsRead(m, n)) break;
      liveHandleMessage(m, n);
      free(m);
    }

    int speakThr = max(12, (int)micNoiseFloor * 3 + 5);
    if (micLevel > speakThr) {
      userSpoke = true;
      lastActivity = millis();
    }
    if (liveModelSpeaking()) lastActivity = millis();
    if ((int32_t)(liveToolDoneAt - lastActivity) > 0) lastActivity = liveToolDoneAt;

    uint32_t now = millis();
    if (asLiveStop || asDismiss) {  // кнопка BOOT или «Стоп» на сайте — замолкаем сразу
      liveFlush = true;
      break;
    }
    if (!liveWs.connected() || liveGoAway || now - t0 > LIVE_MAX_MS) break;
    if (!liveAudioOn) break;  // динамик заняли другим (радио, «Сказать» с сайта) — ответ всё равно не прозвучит
    if (!userSpoke && !liveFirstAudioAt && now - t0 > LIVE_FIRST_WAIT_MS) break;  // никто ничего не сказал
    if (liveEndAfterTurn && liveTurnDone && !liveModelSpeaking()) break;         // включаем музыку
    // Gemini так и не закончил ответ после команды включить музыку — не ждём вечно
    if (liveEndAfterTurn && liveToolDoneAt && now - liveToolDoneAt > 10000 && !liveModelSpeaking()) break;
    if (now - lastActivity > LIVE_IDLE_MS && !liveModelSpeaking()) break;        // разговор закончился
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  Serial.printf("[live] разговор окончен через %lu с\n", (unsigned long)((millis() - t0) / 1000));
  liveFinish();
  return true;
}

void setupLive() {
  liveUplinkHook = liveUplink;
  livePlaybackHook = livePlayback;
  asLiveSessionFn = liveSession;
  if (!liveKey.isEmpty()) snprintf(liveStatus, sizeof(liveStatus), "готов (%s)", liveModel.c_str());
}
