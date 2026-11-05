// Голосовой ассистент:
//   запись до паузы в речи -> распознавание (Groq Whisper) -> ответ языковой модели (Groq) ->
//   выполнение команд (радио, громкость, светодиод) -> озвучка ответа.
// Модель знает показания датчиков, время и прогноз погоды, помнит последние фразы разговора.
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include "audio_player.h"
#include "mic.h"
#include "events.h"
#include "spotify.h"

#define BOOT_PIN 0  // кнопка BOOT на плате

#define GROQ_STT_URL      "https://api.groq.com/openai/v1/audio/transcriptions"
#define GROQ_STT_MODEL    "whisper-large-v3-turbo"
#define GROQ_CHAT_URL     "https://api.groq.com/openai/v1/chat/completions"
#define GROQ_CHAT_MODEL   "openai/gpt-oss-120b"  // можно поменять на странице, если Groq её уберёт

#define WEATHER_LAT  "50.45"   // Киев — поменяй на свой город
#define WEATHER_LON  "30.52"
#define WEATHER_CACHE_MS (30 * 60 * 1000UL)  // прогноз обновляем не чаще раза в 30 минут

#define LISTEN_WAIT_MS    4000  // сколько ждать начала речи после нажатия
#define LISTEN_SILENCE_MS 800   // пауза, после которой считаем, что фраза закончилась
#define FOLLOWUP_WAIT_MS  5000  // после ответа ещё столько ждём продолжения разговора — без «Alexa»

#define RING_REPEATS   3        // сколько раз повторить сигнал таймера или будильника, если его не остановили
#define RING_PAUSE_MS  20000    // пауза между повторами
#define ALARM_RAMP_MS  5000     // будильник с музыкой: громкость растёт на 1 раз в 5 секунд, с тихой до обычной

#define NIGHT_END_HOUR   7      // ночной режим («Спокойной ночи») сам выключается утром в 7:00
#define NIGHT_VOICE_VOL  6      // ночью голос не громче этого

#define AIR_HOLD_MS      (10 * 60 * 1000UL)    // плохой воздух должен продержаться 10 минут, прежде чем скажем
#define AIR_REPEAT_MS    (3 * 3600 * 1000UL)   // о том же — не чаще раза в 3 часа
#define AIR_FROM_HOUR    9      // голосом предупреждаем только днём: с 9:00 до 21:00
#define AIR_TO_HOUR      21

#define HISTORY_PAIRS    6                   // сколько последних вопросов-ответов помнить
#define HISTORY_RESET_MS (5 * 60 * 1000UL)   // после 5 минут тишины разговор начинается заново

// Всё это объявлено в основном скетче
extern Preferences prefs;
extern bool ledEnabled;
extern float curTemp, curHum, curPressure;
bool pressureTrend(float &dp);
const char *forecastText(bool ok, float dp);

enum AssistantState : uint8_t { AS_IDLE, AS_LISTENING, AS_THINKING };

volatile AssistantState asState = AS_IDLE;
char asHeard[512] = "";    // последний вопрос
char asAnswer[2048] = "";  // последний ответ (рецепт может быть длинным)
char asError[200] = "";    // последняя ошибка (пусто — всё хорошо)
String groqKey;            // API-ключ Groq, задаётся на веб-странице
String asModel = GROQ_CHAT_MODEL;
TaskHandle_t asTask = nullptr;

char asTyped[512] = "";         // вопрос, набранный на сайте (вместо голоса)
volatile bool asHasTyped = false;
volatile bool asPending = false; // пришёл новый запрос, пока ассистент договаривал

// Сработавший таймер/будильник/напоминание (кладёт checkEvents(), звонит задача ассистента)
Event asDueEvent;
volatile bool asHasEvent = false;
volatile bool asDismiss = false;  // «Стоп» на сайте — прекратить звонок

time_t nightUntil = 0;     // ночной режим до этого времени (0 — выключен)
time_t sleepStopAt = 0;    // таймер сна: когда выключить музыку (0 — не стоит)
bool airVoice = true;      // предупреждать голосом о плохом воздухе
String airNotice;          // предупреждение о воздухе, которое надо сказать (кладёт checkAir())
volatile bool asHasAir = false;

bool nightActive() { return nightUntil != 0; }

struct AsMsg {
  String role;
  String content;
};
std::vector<AsMsg> asHistory;
uint32_t asLastTalk = 0;

String wxSummary;        // прогноз погоды текстом — для модели
uint32_t wxFetched = 0;

void asSetError(const String &msg) {
  strlcpy(asError, msg.c_str(), sizeof(asError));
  Serial.printf("[assistant] %s\n", asError);
}

// Живой разговор с Gemini (gemini_live.h): если задан — пробуем его первым; false — не вышло, работаем как раньше
bool (*asLiveSessionFn)() = nullptr;
volatile bool asLiveStop = false;  // кнопка BOOT во время живого разговора — закончить его

// Кнопка BOOT или кнопка на сайте: начать слушать, а если уже слушаем — закончить фразу
void assistantTrigger() {
  if (asState == AS_LISTENING) {
    micStopRecording();
    asLiveStop = true;
  } else if (asState == AS_IDLE && asTask) {
    asPending = true;
    xTaskNotifyGive(asTask);
  }
}

// Вопрос текстом с сайта
void assistantAsk(const String &q) {
  if (asState != AS_IDLE || !asTask || q.isEmpty()) return;
  strlcpy(asTyped, q.c_str(), sizeof(asTyped));
  asHasTyped = true;
  asPending = true;
  xTaskNotifyGive(asTask);
}

// ---------- HTTPS ----------

int asHttps(const char *url, const String *jsonBody, String &resp, bool groqAuth) {
  WiFiClientSecure client;
  client.setInsecure();  // без проверки сертификата — для домашнего устройства допустимо
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(20000);
  if (groqAuth) http.addHeader("Authorization", "Bearer " + groqKey);

  int code;
  if (jsonBody) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(*jsonBody);
  } else {
    code = http.GET();
  }
  resp = code > 0 ? http.getString() : HTTPClient::errorToString(code);
  http.end();
  return code;
}

// Понятный текст ошибки из ответа Groq
// Сколько внутренней памяти свободно — без ~40 КБ подряд защищённое соединение не откроется
String heapInfo() {
  return "свободно внутр. памяти " + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
         ", самый большой кусок " + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

String asGroqError(int code, const String &resp) {
  if (code <= 0) {
    Serial.printf("[assistant] %s\n", heapInfo().c_str());
    return "Нет связи с Groq: " + resp;
  }
  JsonDocument doc;
  deserializeJson(doc, resp);
  String msg = doc["error"]["message"] | resp.substring(0, 120);
  return String("Groq ответил ") + code + ": " + msg;
}

// ---------- Поиск радио в каталоге Radio Browser ----------
// Бесплатный каталог десятков тысяч станций, без ключей: radio-browser.info

struct FoundStation {
  String name;
  String url;
};
std::vector<FoundStation> radioFound;  // результаты последнего поиска
size_t radioFoundIdx = 0;              // какая из них играет — для «давай другую»

String urlEncode(const String &s) {
  String out;
  char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// Ищет станции, самые популярные — первыми. Возвращает, сколько нашлось.
int radioSearch(const String &tag, const String &name, const String &countrycode, const String &language) {
  String query = "/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=30";
  if (tag.length()) query += "&tag=" + urlEncode(tag);
  if (name.length()) query += "&name=" + urlEncode(name);
  if (countrycode.length()) query += "&countrycode=" + urlEncode(countrycode);
  if (language.length()) query += "&language=" + urlEncode(language);

  // У каталога несколько зеркал — если одно не отвечает, пробуем следующее
  static const char *mirrors[] = {"de1.api.radio-browser.info", "nl1.api.radio-browser.info",
                                  "at1.api.radio-browser.info", "all.api.radio-browser.info"};
  String resp;
  int code = 0;
  for (const char *host : mirrors) {
    code = asHttps((String("https://") + host + query).c_str(), nullptr, resp, false);
    if (code == 200) break;
  }
  radioFound.clear();
  radioFoundIdx = 0;
  if (code != 200) {
    Serial.printf("[radio] каталог не ответил (%d)\n", code);
    return 0;
  }

  // Из ответа берём только нужные поля — так меньше памяти
  JsonDocument filter;
  filter[0]["name"] = true;
  filter[0]["url_resolved"] = true;
  filter[0]["codec"] = true;
  filter[0]["hls"] = true;
  filter[0]["bitrate"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) return 0;

  for (JsonObject s : doc.as<JsonArray>()) {
    String codec = s["codec"] | "";
    codec.toUpperCase();
    bool playable = codec == "MP3" || codec.startsWith("AAC") || codec == "OGG";
    if (!playable || (s["hls"] | 0) == 1 || (s["bitrate"] | 0) > 320) continue;  // такие плата не потянет
    String url = s["url_resolved"] | "";
    String nm = s["name"] | "";
    nm.trim();
    if (url.isEmpty()) continue;
    radioFound.push_back({nm, url});
    if (radioFound.size() >= 10) break;
  }
  Serial.printf("[radio] поиск tag='%s' name='%s' country='%s' lang='%s': найдено %u\n",
                tag.c_str(), name.c_str(), countrycode.c_str(), language.c_str(), (unsigned)radioFound.size());
  return radioFound.size();
}

// ---------- Соединение с Groq ----------
// Распознавание речи и ответ модели идут на один сервер — держим соединение открытым на время
// разговора (экономит ~0.3–0.5 с на каждом запросе) и закрываем после: TLS занимает много памяти.

WiFiClientSecure groqClient;
HTTPClient groqHttp;
bool groqClientReady = false;

void groqClose() {
  groqClient.stop();
}

// POST с повтором: если открытое соединение успело «протухнуть», второй раз подключаемся заново
int groqPost(const char *url, const String &contentType, const uint8_t *body, size_t len, String &resp,
             uint32_t timeoutMs = 20000) {
  if (!groqClientReady) {
    groqClient.setInsecure();
    groqClientReady = true;
  }
  int code = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    groqHttp.setReuse(true);
    groqHttp.begin(groqClient, url);
    groqHttp.setTimeout(timeoutMs);
    groqHttp.addHeader("Authorization", "Bearer " + groqKey);
    groqHttp.addHeader("Content-Type", contentType);
    code = groqHttp.POST((uint8_t *)body, len);
    if (code > 0) {
      resp = groqHttp.getString();
      groqHttp.end();
      return code;
    }
    resp = HTTPClient::errorToString(code);
    groqHttp.end();
    groqClient.stop();
  }
  return code;
}

// ---------- Запись и распознавание ----------

// Пишем, пока человек говорит. false — если так ничего и не сказали.
bool asListen(uint32_t waitMs = LISTEN_WAIT_MS) {
  sendAudioCmd(AUDIO_STOP);  // чтобы микрофон не слушал радио; radioUrl не трогаем — потом включим обратно
  vTaskDelay(pdMS_TO_TICKS(150));

  int floorLvl = (int)micNoiseFloor;
  int speakThr = max(12, floorLvl * 3 + 5);   // громче этого — речь
  int quietThr = max(6, floorLvl * 2 + 3);    // тише этого — тишина
  Serial.printf("[assistant] слушаю (фон %d, речь > %d, тишина < %d)\n", floorLvl, speakThr, quietThr);

  micStartRecording(MIC_MAX_SECONDS);
  bool heard = false;
  uint32_t start = millis(), lastLoud = millis();
  while (micRecording) {
    vTaskDelay(pdMS_TO_TICKS(30));
    int lvl = micLevel;
    if (lvl > speakThr) heard = true;
    if (lvl > quietThr) lastLoud = millis();
    if (!heard && millis() - start > waitMs) {
      micStopRecording();
      return false;
    }
    if (heard && millis() - lastLoud > LISTEN_SILENCE_MS) micStopRecording();
  }
  return heard || micSamples > MIC_RATE / 2;  // остановили кнопкой — всё равно отправляем
}

// Whisper иногда «слышит» в тишине фразы из субтитров — отбрасываем их
bool asIsHallucination(const String &t) {
  static const char *junk[] = {"Субтитры", "субтитры", "Продолжение следует", "Спасибо за просмотр", "DimaTorzok"};
  for (const char *j : junk)
    if (t.indexOf(j) >= 0) return true;
  return t.length() < 2;
}

// Если запись успела захватить само «Алекса» — убираем его из начала фразы
void asStripWakeWord(String &t) {
  static const char *words[] = {"Алекса", "алекса", "Алекс", "алекс", "Alexa", "alexa", "Элекса", "элекса", "Олекса", "олекса"};
  t.trim();
  for (const char *w : words) {
    if (t.startsWith(w)) {
      t = t.substring(strlen(w));
      while (t.length() && strchr(" ,.!?-—", t[0])) t = t.substring(1);  // пробелы, запятая, точка после слова
      t.trim();
      return;
    }
  }
}

// Отправляем запись в Groq, получаем текст
bool asTranscribe(String &text) {
  const char *boundary = "----meteoBoundary7MA4YWxkTrZu0gW";
  String head;
  auto field = [&](const char *name, const char *value) {
    head += String("--") + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
  };
  field("model", GROQ_STT_MODEL);
  field("language", "ru");
  field("response_format", "json");
  head += String("--") + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
          "Content-Type: audio/wav\r\n\r\n";
  String tail = String("\r\n--") + boundary + "--\r\n";

  // Собираем всё тело запроса одним куском в PSRAM
  uint32_t pcmBytes = micSamples * 2;
  size_t total = head.length() + 44 + pcmBytes + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body) {
    asSetError("Не хватило памяти под запрос");
    return false;
  }
  uint8_t *p = body;
  memcpy(p, head.c_str(), head.length()); p += head.length();
  micWavHeader(p, pcmBytes);              p += 44;
  memcpy(p, micBuf, pcmBytes);            p += pcmBytes;
  memcpy(p, tail.c_str(), tail.length());

  uint32_t t0 = millis();
  String resp;
  int code = groqPost(GROQ_STT_URL, String("multipart/form-data; boundary=") + boundary, body, total, resp);
  free(body);

  if (code != 200) {
    asSetError(asGroqError(code, resp));
    return false;
  }
  JsonDocument doc;
  deserializeJson(doc, resp);
  text = doc["text"] | "";
  text.trim();
  Serial.printf("[assistant] распознано за %lu мс: %s\n", (unsigned long)(millis() - t0), text.c_str());
  return true;
}

// ---------- Что модель знает о мире ----------

String asNowText() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return "неизвестно (часы ещё не синхронизированы с интернетом)";
  static const char *wd[] = {"воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота"};
  static const char *mo[] = {"января", "февраля", "марта", "апреля", "мая", "июня",
                             "июля", "августа", "сентября", "октября", "ноября", "декабря"};
  char buf[96];
  snprintf(buf, sizeof(buf), "%s, %d %s %d года, %02d:%02d",
           wd[t.tm_wday], t.tm_mday, mo[t.tm_mon], t.tm_year + 1900, t.tm_hour, t.tm_min);
  return buf;
}

// Коды погоды Open-Meteo (WMO) -> слова
const char *wmoText(int c) {
  switch (c) {
    case 0: return "ясно";
    case 1: return "преимущественно ясно";
    case 2: return "переменная облачность";
    case 3: return "пасмурно";
    case 45: case 48: return "туман";
    case 51: case 53: case 55: return "морось";
    case 56: case 57: return "ледяная морось";
    case 61: return "небольшой дождь";
    case 63: return "дождь";
    case 65: return "сильный дождь";
    case 66: case 67: return "ледяной дождь";
    case 71: return "небольшой снег";
    case 73: return "снег";
    case 75: return "сильный снег";
    case 77: return "снежная крупа";
    case 80: return "небольшой ливень";
    case 81: return "ливень";
    case 82: return "сильный ливень";
    case 85: case 86: return "снегопад";
    case 95: return "гроза";
    case 96: case 99: return "гроза с градом";
    default: return "погода неизвестна";
  }
}

// Прогноз с Open-Meteo на сегодня, завтра и послезавтра
void asUpdateWeather() {
  if (!wxSummary.isEmpty() && millis() - wxFetched < WEATHER_CACHE_MS) return;

  const char *url =
    "https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,wind_speed_10m_max"
    "&timezone=Europe%2FKyiv&forecast_days=3&wind_speed_unit=ms";
  String resp;
  int code = asHttps(url, nullptr, resp, false);
  JsonDocument d;
  if (code != 200 || deserializeJson(d, resp)) {
    Serial.printf("[assistant] прогноз погоды не получен (%d), %s\n", code, heapInfo().c_str());
    return;  // оставляем старый прогноз, если был
  }

  JsonObject cur = d["current"];
  String s = "Сейчас на улице: " + String(lroundf(cur["temperature_2m"] | 0.0f)) + " °C, " +
             wmoText(cur["weather_code"] | -1) + ", влажность " + String(cur["relative_humidity_2m"] | 0) +
             " %, ветер " + String(lroundf(cur["wind_speed_10m"] | 0.0f)) + " м/с.";

  static const char *days[] = {"Сегодня", "Завтра", "Послезавтра"};
  JsonObject daily = d["daily"];
  for (int i = 0; i < 3; i++) {
    s += String(" ") + days[i] + ": " + wmoText(daily["weather_code"][i] | -1) +
         ", от " + String(lroundf(daily["temperature_2m_min"][i] | 0.0f)) +
         " до " + String(lroundf(daily["temperature_2m_max"][i] | 0.0f)) + " °C" +
         ", вероятность осадков " + String(daily["precipitation_probability_max"][i] | 0) + " %" +
         ", ветер до " + String(lroundf(daily["wind_speed_10m_max"][i] | 0.0f)) + " м/с.";
  }
  wxSummary = s;
  wxFetched = millis();
  Serial.printf("[assistant] прогноз: %s\n", s.c_str());
}

String asContextText();

String asSystemPrompt() {
  String s =
    "Ты — голосовой помощник по имени Ассистент, как Алиса или Siri, живёшь в умной колонке-метеостанции "
    "в квартире в Киеве. Ты универсальный помощник: отвечаешь на любые вопросы из своих знаний — рецепты, "
    "советы, факты, объяснения, история, математика, переводы, анекдоты, сказки для ребёнка и всё остальное. "
    "Не отказывайся отвечать только потому, что вопрос не про погоду или колонку. "
    "Отвечай по-русски, вежливо. Обычно коротко — 1–3 предложения. Но если по сути нужен подробный ответ "
    "(рецепт, инструкция, сказка, объяснение по шагам) — отвечай полностью, до 10–12 предложений, "
    "шаги рецепта или инструкции называй словами по порядку: «сначала…, затем…, после этого…». "
    "Ответ будет озвучен синтезатором речи, поэтому пиши разговорным текстом, как говорят вслух: "
    "простые предложения с точками и запятыми, без кавычек, тире, скобок, многоточий, двоеточий и сокращений "
    "(не «т. е.», «и т. д.», «г.», «руб.» — пиши словами). "
    "Не используй списки, разметку, эмодзи и ссылки; единицы измерения пиши словами и согласовывай "
    "с числом (например: 25 градусов, 44 процента, 752 миллиметра ртутного столба, 200 граммов муки). "
    "Если распознанная фраза бессмысленна, вежливо попроси повторить. "
    "У тебя есть поиск в интернете (команда web_search ниже) — используй его для всего свежего и меняющегося: "
    "новости, курсы валют, цены, результаты матчей, расписания, события, «что сейчас происходит», а также если "
    "не уверен в факте. Для общих знаний (рецепты, советы, история, объяснения) поиск не нужен — отвечай сам. "
    "Погоду в Киеве бери из данных ниже, без поиска. "
    "Если просят управлять тем, чего у тебя нет (свет, телевизор, другие устройства), честно скажи, что пока не умеешь. "
    "На «доброе утро» отвечай утренней сводкой в 3–4 предложениях: поздоровайся, назови день недели, дату и время, "
    "погоду на сегодня в Киеве (температура, осадки, ветер) и коротко — температуру в комнате.\n\n";

  s += asContextText();
  s += "\n"
       "Отвечай строго JSON-объектом вида {\"say\": \"текст ответа\", \"actions\": [...]}. "
       "actions — список команд, которые нужно выполнить (пустой, если команд нет):\n"
       "{\"do\": \"radio_play\", \"station\": \"id\"} — включить одну из любимых станций "
       "(на просто «включи радио» или «включи музыку» — hitfm);\n"
       "{\"do\": \"radio_search\", \"tag\": \"...\", \"name\": \"...\", \"countrycode\": \"...\", \"language\": \"...\"} — "
       "найти и включить станцию в мировом каталоге радио, если просят жанр, настроение, страну, язык "
       "или станцию не из любимых. tag — жанр или настроение одним словом по-английски маленькими буквами "
       "(jazz, rock, pop, classical, lofi, chillout, ambient, dance, metal, blues, 80s, 90s, news и т. п.); "
       "name — часть названия станции; countrycode — двухбуквенный код страны (UA, PL, US, IT…); "
       "language — язык по-английски маленькими буквами (ukrainian, english, french…). "
       "Заполняй только нужные поля, остальные не указывай. Конкретную песню включить нельзя — "
       "в этом случае предложи станцию подходящего жанра;\n"
       "{\"do\": \"radio_next\"} — включить следующую станцию из последнего поиска («давай другую», «не то»);\n"
       "{\"do\": \"radio_stop\"} — выключить радио;\n"
       "{\"do\": \"volume\", \"value\": число от 0 до 21} — громкость музыки; на «тише» или «громче» (про музыку, радио) "
       "меняй на 3;\n"
       "{\"do\": \"voice_volume\", \"value\": число от 0 до 21} — громкость твоего собственного голоса; "
       "на «говори громче», «говори тише», «тебя плохо слышно» меняй на 3;\n"
       "{\"do\": \"led\", \"on\": true или false} — включить или выключить светодиод;\n"
       "{\"do\": \"timer\", \"seconds\": число, \"label\": \"...\"} — поставить таймер (label — необязательная подпись, "
       "например «паста»);\n"
       "{\"do\": \"alarm\", \"time\": \"ЧЧ:ММ\", \"repeat\": \"once\", \"daily\" или \"weekdays\", "
       "\"station\": \"id\"} — поставить будильник (once — один раз, в ближайшее такое время). "
       "station — необязательно: если просят разбудить музыкой или радио, id любимой станции (на просто «с музыкой» — hitfm); "
       "тогда после утренней сводки плавно заиграет эта станция;\n"
       "{\"do\": \"good_night\", \"minutes\": число} — на «спокойной ночи», «я спать», «мы ложимся»: ночной режим "
       "до 7 утра (светодиод гаснет, голос тише, без предупреждений). minutes — сколько ещё играть музыке, если просят "
       "«поиграй ещё полчаса»; иначе 0 — музыка выключится сразу. В say пожелай спокойной ночи, коротко;\n"
       "{\"do\": \"sleep_timer\", \"minutes\": число} — выключить музыку через столько минут («выключи музыку через "
       "20 минут»); 0 — отменить таймер сна;\n"
       "{\"do\": \"good_morning\"} — выключить ночной режим, если просят («мы проснулись», «выключи ночной режим»);\n"
       "{\"do\": \"reminder\", \"at\": \"ГГГГ-ММ-ДД ЧЧ:ММ\", \"text\": \"...\"} — напоминание; время считай сам от "
       "текущего (например, «через 2 часа», «завтра в 9»), text — о чём напомнить, коротко;\n"
       "{\"do\": \"cancel\", \"id\": номер} — отменить таймер, будильник или напоминание по номеру из списка выше;\n"
       "{\"do\": \"cancel_all\", \"type\": \"timer\", \"alarm\", \"reminder\" или \"all\"} — отменить все такого вида;\n"
       "{\"do\": \"web_search\", \"query\": \"...\"} — найти ответ в интернете; query — подробный поисковый запрос "
       "(с датой, городом, страной, если важно). В say при этом коротко: «Секунду, сейчас поищу». "
       "Ответ из интернета озвучится сам после поиска.\n"
       "На вопросы «сколько осталось», «какие у меня будильники» отвечай по списку выше. "
       "Выполняя команду, коротко подтверди её в say (например: «Включаю Хіт FM», «Ставлю таймер на 10 минут»).";
  if (spotifyReady())
    s += "\nПодключён Spotify. Конкретную песню, исполнителя, альбом или плейлист включай в Spotify — это "
         "главнее правила «конкретную песню включить нельзя». Радио (radio_*) — только когда просят радио "
         "или станцию. Spotify играет на устройстве пользователя (телефон, компьютер), где открыт Spotify.\n"
         "{\"do\": \"spotify_play\", \"query\": \"...\", \"type\": \"track\", \"artist\", \"album\" или \"playlist\"} — "
         "найти и включить; query — название и исполнитель как сказали («Обійми Океан Ельзи»); на «включи "
         "такого-то исполнителя» — type artist;\n"
         "{\"do\": \"spotify_control\", \"action\": \"pause\", \"resume\", \"next\" или \"previous\"} — пауза, "
         "продолжить, следующий, предыдущий трек в Spotify;\n"
         "{\"do\": \"spotify_now\"} — узнать, что играет в Spotify (название озвучится само).\n"
         "Будильник может разбудить и музыкой из Spotify: в alarm вместо station укажи \"spotify\": \"что включить\" "
         "и \"spotify_type\": \"track\", \"artist\", \"album\" или \"playlist\".";
  return s;
}

// Что колонка знает о мире прямо сейчас: время, датчики, погода, радио, будильники — общее для обоих режимов
String asContextText() {
  float dp = 0;
  bool trendOk = pressureTrend(dp);
  String s;
  s += "Сейчас: " + asNowText() + ".\n";
  s += "В комнате (датчики метеостанции): температура " + String(curTemp, 1) + " °C, влажность " +
       String(lroundf(curHum)) + " %, давление " + String(lroundf(curPressure * 0.750062f)) +
       " мм рт. ст. (" + String(curPressure, 1) + " гПа).";
  if (trendOk) s += " За последние 3 часа давление изменилось на " + String(dp, 1) + " гПа.";
  s += String(" Прогноз по давлению: ") + forecastText(trendOk, dp) + ".\n";
  s += "Прогноз погоды в Киеве: " + (wxSummary.isEmpty() ? String("сейчас недоступен.") : wxSummary) + "\n\n";

  String playing = "выключено";
  if (radioUrl[0]) {
    playing = String("играет станция «") + (radioName[0] ? radioName : "без названия") + "»";
    if (nowTitle[0]) playing += String(", сейчас звучит: ") + nowTitle;
  }
  s += "Состояние: радио " + playing + "; громкость музыки " + String(volume) + " из 21; громкость твоего голоса " +
       String(voiceVolume) + " из 21; светодиод " +
       (ledEnabled ? "включён" : "выключен") + ".\n";
  s += "Таймеры, будильники и напоминания (#номер — описание): " + eventsText() + "\n";
  if (nightActive()) s += "Сейчас ночной режим (сказали «спокойной ночи»): в доме спят, отвечай тихо и коротко.\n";
  if (sleepStopAt) s += "Таймер сна: музыка выключится в " + formatClock(sleepStopAt, false) + ".\n";

  s += "Любимые станции (id — название): ";
  for (size_t i = 0; i < STATION_COUNT; i++) s += String(STATIONS[i].id) + " — " + STATIONS[i].name + "; ";
  s += "\n";
  return s;
}

// ---------- Разговор с моделью ----------

bool asChat(const String &question, String &content) {
  if (millis() - asLastTalk > HISTORY_RESET_MS) asHistory.clear();

  JsonDocument req;
  req["model"] = asModel;
  req["temperature"] = 0.5;
  // gpt-oss сначала «думает», и эти размышления тоже расходуют лимит токенов —
  // поэтому лимит с запасом, а думать просим поменьше, чтобы ответ приходил быстрее
  req["max_tokens"] = 1500;
  if (asModel.startsWith("openai/gpt-oss")) req["reasoning_effort"] = "low";
  req["response_format"]["type"] = "json_object";
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = asSystemPrompt();
  for (const AsMsg &m : asHistory) {
    JsonObject o = msgs.add<JsonObject>();
    o["role"] = m.role;
    o["content"] = m.content;
  }
  JsonObject user = msgs.add<JsonObject>();
  user["role"] = "user";
  user["content"] = question;

  String body, resp;
  serializeJson(req, body);
  uint32_t t0 = millis();
  int code = groqPost(GROQ_CHAT_URL, "application/json", (const uint8_t *)body.c_str(), body.length(), resp);
  if (code != 200) {
    asSetError(asGroqError(code, resp));
    return false;
  }

  JsonDocument doc;
  deserializeJson(doc, resp);
  content = doc["choices"][0]["message"]["content"] | "";
  Serial.printf("[assistant] ответ за %lu мс: %s\n", (unsigned long)(millis() - t0), content.c_str());

  asHistory.push_back({"user", question});
  asHistory.push_back({"assistant", content});
  while (asHistory.size() > HISTORY_PAIRS * 2) asHistory.erase(asHistory.begin(), asHistory.begin() + 2);
  asLastTalk = millis();
  return true;
}

bool asMusicChanged = false;  // в этом ответе включили радио — после него не переспрашиваем, а играем музыку
String asWebQuery;            // модель попросила поискать в интернете — что именно

// ---------- Поиск в интернете ----------
// Встроенный поиск Groq (browser_search) не работает вместе с JSON-ответами, поэтому это отдельный
// второй запрос: обычный текстовый ответ модели, которая сама сходила в интернет.

// Убираем то, что плохо звучит вслух: пометки источников 【…】, ссылки, разметку
String asCleanForSpeech(String s) {
  int a;
  while ((a = s.indexOf("【")) >= 0) {
    int b = s.indexOf("】", a);
    if (b < 0) break;
    s.remove(a, b - a + strlen("】"));
  }
  while ((a = s.indexOf("http")) >= 0) {
    int b = a;
    while (b < (int)s.length() && s[b] != ' ' && s[b] != ')' && s[b] != '\n') b++;
    s.remove(a, b - a);
  }
  s.replace("**", "");
  s.replace("*", "");
  s.replace("#", "");
  s.replace("()", "");
  s.replace("\n", " ");
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  s.trim();
  return s;
}

String asWebSearch(const String &query, const String &question) {
  String model = asModel.startsWith("openai/gpt-oss") ? asModel : String("openai/gpt-oss-20b");  // поиск есть только у них
  JsonDocument req;
  req["model"] = model;
  req["temperature"] = 0.5;
  req["max_completion_tokens"] = 2048;
  req["reasoning_effort"] = "low";  // по совету Groq: иначе поиск дольше и дороже
  req["tool_choice"] = "required";
  req["tools"][0]["type"] = "browser_search";
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = "Ты голосовой помощник. Найди ответ в интернете и ответь по-русски для озвучивания вслух: "
                   "2–4 предложения, самое главное, без ссылок, списков, разметки и указания источников. "
                   "Числа и единицы пиши так, чтобы их было удобно произнести. Сейчас: " + asNowText() + ".";
  JsonObject user = msgs.add<JsonObject>();
  user["role"] = "user";
  user["content"] = "Вопрос: " + question + "\nЧто искать: " + query;

  String body, resp;
  serializeJson(req, body);
  uint32_t t0 = millis();
  int code = groqPost(GROQ_CHAT_URL, "application/json", (const uint8_t *)body.c_str(), body.length(), resp, 45000);
  if (code != 200) {
    asSetError(asGroqError(code, resp));
    return "Извините, не получилось поискать в интернете.";
  }
  JsonDocument doc;
  deserializeJson(doc, resp);
  String answer = asCleanForSpeech(doc["choices"][0]["message"]["content"] | "");
  Serial.printf("[assistant] поиск «%s» за %lu мс: %s\n", query.c_str(), (unsigned long)(millis() - t0), answer.c_str());
  return answer.isEmpty() ? String("Ничего не нашёл, попробуй спросить по-другому.") : answer;
}

// ---------- Ночной режим, таймер сна ----------

// Выключить музыку совсем (если играл поток Spotify с моста — ставим Spotify на паузу, иначе он так и будет играть на ПК)
void asStopMusic() {
  if (!spStreamUrl.isEmpty() && spStreamUrl == radioUrl) spotifyControl("pause");
  radioUrl[0] = 0;
  radioName[0] = 0;
  nowTitle[0] = 0;
}

void nightStart() {
  nightUntil = eventNextAt(NIGHT_END_HOUR, 0, REP_ONCE, time(nullptr));
  if (voiceVolume > NIGHT_VOICE_VOL) audioSetVoiceVolume(NIGHT_VOICE_VOL);  // в память не пишем — утром вернём
  Serial.printf("[assistant] ночной режим до %s\n", formatClock(nightUntil, true).c_str());
}

void nightEnd() {
  if (!nightActive()) return;
  nightUntil = 0;
  audioSetVoiceVolume(prefs.getUChar("vvol", voiceVolume));  // дневная громкость голоса
  Serial.println("[assistant] ночной режим выключен");
}

// Раз в минуту (из задачи ассистента): утро — выключить ночной режим; таймер сна — плавно убрать музыку
void nightTick() {
  time_t now = time(nullptr);
  if (nightActive() && now >= nightUntil) nightEnd();
  if (!sleepStopAt || now < sleepStopAt) return;
  sleepStopAt = 0;
  if (!radioUrl[0]) return;
  Serial.println("[assistant] таймер сна — выключаю музыку");
  uint8_t v0 = volume;
  for (int v = v0 - 1; v >= 0 && radioUrl[0] && !asPending; v--) {  // минуту-полторы тише и тише
    audioSetVolume(v);
    vTaskDelay(pdMS_TO_TICKS(4000));
  }
  if (radioUrl[0] && !asPending) {
    asStopMusic();
    sendAudioCmd(AUDIO_STOP);
  }
  audioSetVolume(v0);  // в следующий раз музыка заиграет с обычной громкостью
}

// ---------- Предупреждения о воздухе в комнате ----------

String ruPlural(long n, const char *one, const char *few, const char *many) {
  long a = labs(n) % 100, b = a % 10;
  return String(n) + " " + (a >= 11 && a <= 19 ? many : b == 1 ? one : b >= 2 && b <= 4 ? few : many);
}

// Пороги — с запасом от светодиода (он меняет цвет раньше), чтобы не говорить по пустякам
enum AirKind : uint8_t { AIR_DRY, AIR_DAMP, AIR_COLD, AIR_HOT, AIR_STORM, AIR_KINDS };

bool airBad(uint8_t k, float dp, bool trendOk) {
  switch (k) {
    case AIR_DRY:   return curHum < 30;
    case AIR_DAMP:  return curHum > 70;
    case AIR_COLD:  return curTemp < 17;
    case AIR_HOT:   return curTemp > 28;
    case AIR_STORM: return trendOk && dp <= -3;  // за 3 часа упало на 3 гПа и больше
  }
  return false;
}

String airSay(uint8_t k) {  // для голоса — полной фразой
  long h = lroundf(curHum), t = lroundf(curTemp);
  switch (k) {
    case AIR_DRY:   return "В комнате сухой воздух, влажность " + ruPlural(h, "процент", "процента", "процентов") +
                           ". Хорошо бы включить увлажнитель.";
    case AIR_DAMP:  return "В комнате очень влажно, " + ruPlural(h, "процент", "процента", "процентов") +
                           ". Стоит проветрить.";
    case AIR_COLD:  return "В комнате прохладно, " + ruPlural(t, "градус", "градуса", "градусов") + ".";
    case AIR_HOT:   return "В комнате жарко, " + ruPlural(t, "градус", "градуса", "градусов") + ". Можно проветрить.";
    case AIR_STORM: return "Давление быстро падает, возможна непогода.";
  }
  return "";
}

String airStatus() {  // для страницы — коротко
  static const char *t[] = {"сухой воздух — включи увлажнитель", "очень влажно — проветри", "прохладно",
                            "жарко — проветри", "давление быстро падает — возможна непогода"};
  float dp = 0;
  bool ok = pressureTrend(dp);
  String s;
  for (uint8_t k = 0; k < AIR_KINDS; k++)
    if (airBad(k, dp, ok)) s += (s.isEmpty() ? "" : "; ") + String(t[k]);
  return s;
}

// Из loop(): если воздух плохой уже 10 минут, сейчас день и колонка свободна — просим ассистента сказать
void checkAir() {
  static uint32_t last = 0, since[AIR_KINDS] = {}, said[AIR_KINDS] = {};
  static bool everSaid[AIR_KINDS] = {};
  if (millis() - last < 30000) return;
  last = millis();
  float dp = 0;
  bool ok = pressureTrend(dp);
  bool due[AIR_KINDS] = {};
  String notice;
  for (uint8_t k = 0; k < AIR_KINDS; k++) {
    if (!airBad(k, dp, ok)) {
      since[k] = 0;
      continue;
    }
    if (!since[k]) since[k] = millis() | 1;  // 0 — значит «всё хорошо»
    due[k] = millis() - since[k] >= AIR_HOLD_MS && (!everSaid[k] || millis() - said[k] >= AIR_REPEAT_MS);
    if (due[k]) notice += (notice.isEmpty() ? "" : " ") + airSay(k);
  }
  if (notice.isEmpty() || !airVoice || nightActive() || !timeValid() || !asTask) return;
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  if (t.tm_hour < AIR_FROM_HOUR || t.tm_hour >= AIR_TO_HOUR) return;
  if (asState != AS_IDLE || asHasAir || asHasEvent || liveAudioOn || micRecording) return;
  for (uint8_t k = 0; k < AIR_KINDS; k++)
    if (due[k]) {
      said[k] = millis();
      everSaid[k] = true;
    }
  airNotice = notice;
  asHasAir = true;
  xTaskNotifyGive(asTask);
}

// Выполняет команду. Возвращает текст, которым нужно заменить ответ, если команда не удалась.
String asRunAction(JsonObject a) {
  String what = a["do"] | "";
  Serial.printf("[assistant] команда: %s\n", what.c_str());

  if (what.startsWith("radio_") && what != "radio_stop") asMusicChanged = true;

  bool needsClock = what == "timer" || what == "alarm" || what == "reminder";
  if (needsClock && !timeValid())
    return "Не могу: часы ещё не синхронизировались с интернетом. Попробуй через минуту.";

  if (what == "timer") {
    long sec = a["seconds"] | 0;
    if (sec <= 0) return "Не понял, на сколько поставить таймер.";
    if (!eventAdd(EV_TIMER, time(nullptr) + sec, REP_ONCE, sec, a["label"] | ""))
      return "Не получилось: слишком много таймеров и будильников.";
  } else if (what == "alarm") {
    int h = -1, m = -1;
    sscanf(a["time"] | "", "%d:%d", &h, &m);
    if (h < 0 || h > 23 || m < 0 || m > 59) return "Не понял, на какое время поставить будильник.";
    String rep = a["repeat"] | "once";
    uint8_t repeat = rep == "daily" ? REP_DAILY : rep == "weekdays" ? REP_WEEKDAYS : REP_ONCE;
    // Музыка будильника: любимая станция или что-то из Spotify
    String music, station = a["station"] | "", sp = a["spotify"] | "";
    if (!sp.isEmpty() && spotifyReady()) music = "s:" + String(a["spotify_type"] | "track") + ":" + sp;
    else if (!station.isEmpty()) music = "r:" + station;
    else if (!sp.isEmpty()) music = "r:hitfm";  // Spotify не подключён — будим радио
    if (!eventAdd(EV_ALARM, eventNextAt(h, m, repeat, time(nullptr)), repeat, 0, music))
      return "Не получилось: слишком много таймеров и будильников.";
  } else if (what == "reminder") {
    struct tm t = {};
    if (sscanf(a["at"] | "", "%d-%d-%d %d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min) != 5)
      return "Не понял, когда напомнить.";
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = -1;
    time_t when = mktime(&t);
    if (when <= time(nullptr)) return "Это время уже прошло. Скажи, когда напомнить.";
    if (!eventAdd(EV_REMINDER, when, REP_ONCE, 0, a["text"] | ""))
      return "Не получилось: слишком много напоминаний.";
  } else if (what == "cancel") {
    if (!eventCancel(a["id"] | 0)) return "Такого таймера или будильника нет.";
  } else if (what == "cancel_all") {
    String type = a["type"] | "all";
    eventCancelAll(type == "timer" ? EV_TIMER : type == "alarm" ? EV_ALARM : type == "reminder" ? EV_REMINDER : -1);
  }

  // Радио включится после того, как ассистент договорит
  if (what == "radio_play") {
    String id = a["station"] | "hitfm";
    const Station *st = &STATIONS[0];
    for (size_t i = 0; i < STATION_COUNT; i++)
      if (id == STATIONS[i].id) st = &STATIONS[i];
    radioSet(st->url, st->name);
  } else if (what == "radio_search") {
    int n = radioSearch(a["tag"] | "", a["name"] | "", a["countrycode"] | "", a["language"] | "");
    if (n == 0) return "К сожалению, не нашёл подходящую станцию. Попробуй сказать по-другому.";
    radioSet(radioFound[0].url, radioFound[0].name);
  } else if (what == "radio_next") {
    if (radioFound.size() < 2) return "Других станций в последнем поиске нет. Скажи, какую музыку включить.";
    radioFoundIdx = (radioFoundIdx + 1) % radioFound.size();
    radioSet(radioFound[radioFoundIdx].url, radioFound[radioFoundIdx].name);
  } else if (what == "radio_stop") {
    asStopMusic();
    sleepStopAt = 0;
  } else if (what == "good_night") {
    nightStart();
    long min = a["minutes"] | 0;
    if (min > 0 && radioUrl[0]) sleepStopAt = time(nullptr) + min * 60;
    else asStopMusic();
  } else if (what == "good_morning") {
    nightEnd();
  } else if (what == "sleep_timer") {
    long min = a["minutes"] | 0;
    if (min > 0 && !radioUrl[0]) return "Сейчас ничего не играет.";
    sleepStopAt = min > 0 ? time(nullptr) + min * 60 : 0;
  } else if (what == "volume") {
    audioSetVolume(a["value"] | (int)volume);
    prefs.putUChar("vol", volume);
  } else if (what == "voice_volume") {
    audioSetVoiceVolume(a["value"] | (int)voiceVolume);
    prefs.putUChar("vvol", voiceVolume);
  } else if (what == "led") {
    ledEnabled = a["on"] | true;
    prefs.putBool("led", ledEnabled);
  } else if (what == "web_search") {
    asWebQuery = a["query"] | "";
  } else if (what == "spotify_play") {
    String failed = spotifyPlay(a["query"] | "", a["type"] | "track");
    if (!failed.isEmpty()) return failed;
    if (!spStreamUrl.isEmpty()) {
      // Spotify играет на мосту на ПК — колонка после ответа включает его поток, как радио
      radioSet(spStreamUrl, "Spotify");
      asMusicChanged = true;
    } else {
      // Музыка играет на телефоне или ПК — своё радио после разговора не включаем
      radioUrl[0] = 0;
      radioName[0] = 0;
      nowTitle[0] = 0;
    }
  } else if (what == "spotify_control") {
    String act = a["action"] | "";
    String failed = spotifyControl(act);
    if (!failed.isEmpty()) return failed;
    if (act != "pause" && !spStreamUrl.isEmpty() && spStreamUrl != radioUrl) {  // продолжили — слушаем поток моста
      radioSet(spStreamUrl, "Spotify");
      asMusicChanged = true;
    }
  } else if (what == "spotify_now") {
    String failed = spotifyNowPlaying();
    return failed.isEmpty() ? asActionInfo : failed;  // ответом и будет название трека
  }
  return "";
}

// ---------- Основной цикл ассистента ----------

// Спрашивает модель и выполняет команды. Возвращает текст ответа.
String asAsk(const String &question) {
  asUpdateWeather();
  asMusicChanged = false;
  asWebQuery = "";

  String say, content;
  if (asChat(question, content)) {
    JsonDocument ans;
    if (!deserializeJson(ans, content) && ans["say"].is<const char *>()) {
      say = ans["say"].as<String>();
      for (JsonObject a : ans["actions"].as<JsonArray>()) {
        String failed = asRunAction(a);
        if (!failed.isEmpty()) say = failed;
      }
    } else {
      say = content;  // модель ответила не JSON — просто озвучиваем как есть
    }
  } else {
    say = "Извините, не получилось получить ответ.";
  }

  // Нужен интернет: пока ищем, говорим «секунду, поищу», потом — найденный ответ
  if (!asWebQuery.isEmpty()) {
    if (!say.isEmpty()) {
      strlcpy(asAnswer, say.c_str(), sizeof(asAnswer));
      sendAudioCmd(AUDIO_SAY, say);
    }
    say = asWebSearch(asWebQuery, question);
    // В память разговора — найденный ответ, чтобы можно было переспросить
    if (!asHistory.empty()) {
      JsonDocument h;
      h["say"] = say;
      h["actions"].to<JsonArray>();
      String hs;
      serializeJson(h, hs);
      asHistory.back().content = hs;
    }
  }
  return say;
}

// Ждём ms миллисекунд, но прерываемся, если нажали кнопку, сказали «Alexa» или нажали «Стоп»
bool asWaitInterruptible(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (asPending || asDismiss) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return true;
}

// Будильник с музыкой: включить её тихо и плавно сделать громче. false — музыка не заиграла на колонке.
bool asAlarmMusic(const char *m) {
  String s = m;
  if (s.startsWith("s:")) {
    int c = s.indexOf(':', 2);
    JsonDocument act;
    act["do"] = "spotify_play";
    act["type"] = s.substring(2, c);
    act["query"] = s.substring(c + 1);
    if (!asRunAction(act.as<JsonObject>()).isEmpty()) s = "r:hitfm";  // ПК выключен или мост не запущен — будим радио
    else if (!radioUrl[0]) return false;  // Spotify заиграл на другом устройстве
  }
  if (s.startsWith("r:")) {
    String id = s.substring(2);
    const Station *st = &STATIONS[0];
    for (size_t i = 0; i < STATION_COUNT; i++)
      if (id == STATIONS[i].id) st = &STATIONS[i];
    radioSet(st->url, st->name);
  }
  if (!radioUrl[0]) return false;
  int target = volume, v = target < 3 ? target : 3;
  Serial.printf("[assistant] будильник: музыка «%s», громкость %d -> %d\n", radioName, v, target);
  audioSetVolume(v);
  sendAudioCmd(AUDIO_PLAY, radioUrl);
  while (v < target && asWaitInterruptible(ALARM_RAMP_MS)) audioSetVolume(++v);
  return true;
}

// Звонок таймера, будильника или напоминания: сказать, и повторять, пока не остановят.
// true — заиграла музыка будильника (второй раз радио включать не нужно).
bool asRingEvent(const Event &e) {
  String first, again;
  if (e.type == EV_TIMER) {
    first = e.text[0] ? String("Время вышло! Таймер «") + e.text + "» закончился."
                      : "Время вышло! Таймер на " + formatDuration(e.seconds) + " закончился.";
    again = first;
  } else if (e.type == EV_ALARM) {
    nightEnd();  // утро
    // Будильник сразу рассказывает утреннюю сводку
    first = asAsk("Сработал будильник. Скажи, что пора вставать, и дай утреннюю сводку.");
    if (asError[0]) first = "Доброе утро! Пора вставать. Сейчас " + formatClock(time(nullptr), false) + ".";  // нет связи с Groq
    again = "Пора вставать!";
  } else {
    first = String("Напоминание: ") + e.text;
    again = first;
  }
  Serial.printf("[assistant] звонок: %s\n", first.c_str());
  strlcpy(asAnswer, first.c_str(), sizeof(asAnswer));

  asDismiss = false;
  sendAudioCmd(AUDIO_SAY, first);
  if (e.type == EV_ALARM && e.text[0]) {  // будильник с музыкой: после сводки — музыка вместо «Пора вставать!»
    waitAudioDone(60000, &asPending);
    if (asPending || asDismiss) return false;
    return asAlarmMusic(e.text);
  }
  int repeats = e.type == EV_REMINDER ? 1 : RING_REPEATS;
  for (int i = 0; i < repeats; i++) {
    waitAudioDone(60000, &asPending);
    if (!asWaitInterruptible(RING_PAUSE_MS)) break;
    sendAudioCmd(AUDIO_SAY, again);
  }
  if (asDismiss) sendAudioCmd(AUDIO_STOP);
  return false;
}

void assistantTaskFn(void *) {
  for (;;) {
    // Ждём кнопку, «Alexa» или будильник; раз в минуту между делом обновляем прогноз погоды,
    // чтобы в момент вопроса он уже был готов и не добавлял задержку
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000)) == 0) {
      if (WiFi.status() == WL_CONNECTED) asUpdateWeather();
      nightTick();
      continue;
    }
    asPending = false;
    asDismiss = false;
    asLiveStop = false;
    asError[0] = 0;

    bool alarmMusic = false;
    if (asHasEvent) {
      Event e = asDueEvent;
      asHasEvent = false;
      alarmMusic = asRingEvent(e);
    } else if (asHasAir && !asHasTyped) {
      asHasAir = false;
      Serial.printf("[assistant] воздух: %s\n", airNotice.c_str());
      strlcpy(asAnswer, airNotice.c_str(), sizeof(asAnswer));
      sendAudioCmd(AUDIO_SAY, airNotice);
    } else if (!asHasTyped && asLiveSessionFn && asLiveSessionFn()) {
      // Поговорили через Gemini Live — всё сделано, кроме просьбы включить что-то в Spotify
      if (!spPendingQuery.isEmpty()) {
        JsonDocument act;
        act["do"] = "spotify_play";
        act["query"] = spPendingQuery;
        act["type"] = spPendingType;
        spPendingQuery = "";
        String failed = asRunAction(act.as<JsonObject>());
        if (!failed.isEmpty()) {
          strlcpy(asAnswer, failed.c_str(), sizeof(asAnswer));
          sendAudioCmd(AUDIO_SAY, failed);
        }
      }
    } else if (groqKey.isEmpty()) {
      asSetError("Не задан API-ключ Groq — впиши его на странице");
      asHasTyped = false;
    } else {
      // Разговор: после ответа ещё FOLLOWUP_WAIT_MS слушаем продолжение — без повторного «Alexa»
      bool firstTurn = true;
      for (;;) {
        String question;
        bool typed = firstTurn && asHasTyped;
        if (typed) {
          question = asTyped;
          asHasTyped = false;
          asState = AS_THINKING;
        } else {
          asState = AS_LISTENING;
          if (!asListen(firstTurn ? LISTEN_WAIT_MS : FOLLOWUP_WAIT_MS)) {
            if (firstTurn) asSetError("Ничего не услышал");  // а после ответа молчание — просто конец разговора
            break;
          }
          asState = AS_THINKING;
          if (!asTranscribe(question)) break;
          if (asIsHallucination(question)) {
            if (firstTurn) asSetError("Не разобрал, повтори");
            break;
          }
          asStripWakeWord(question);
          if (question.length() < 2) {  // сказали только «Алекса» — ждём сам вопрос
            Serial.println("[assistant] услышал только «Алекса» — слушаю вопрос");
            continue;
          }
        }
        firstTurn = false;
        strlcpy(asHeard, question.c_str(), sizeof(asHeard));
        asAnswer[0] = 0;

        String say = asAsk(question);
        strlcpy(asAnswer, say.c_str(), sizeof(asAnswer));
        if (say.isEmpty()) break;
        sendAudioCmd(AUDIO_SAY, say);
        asState = AS_IDLE;  // пока говорит, можно перебить кнопкой или словом «Alexa»

        // Текстовый вопрос с сайта и включение музыки — без продолжения разговора
        if (typed || asMusicChanged) break;
        waitAudioDone(60000, &asPending);
        if (asPending) break;
      }
      asState = AS_IDLE;
    }
    groqClose();  // разговор окончен — освобождаем память соединения

    // Если радио играло (или его попросили включить) — включаем после ответа.
    // Если за это время снова нажали кнопку — не включаем: сейчас начнём слушать.
    if (radioUrl[0] && !asDismiss && !alarmMusic) {
      waitAudioDone(60000, &asPending);
      if (!asPending && radioUrl[0]) sendAudioCmd(AUDIO_PLAY, radioUrl);
    }
  }
}

// Вызывается из loop(): если подошло время таймера/будильника и ассистент свободен — звоним
void checkEvents() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  if (asState != AS_IDLE || asHasEvent || !asTask) return;
  Event e;
  if (!eventsTakeDue(e)) return;
  asDueEvent = e;
  asHasEvent = true;
  asPending = true;  // прерываем ожидание, если ассистент сейчас договаривает
  xTaskNotifyGive(asTask);
}

// «Стоп» на сайте: прекратить звонок и замолчать
void assistantDismiss() {
  asDismiss = true;
  sendAudioCmd(AUDIO_STOP);
}

void wifiRequestSetup();  // wifi_setup.h

// Нажали — ассистент слушает; держат 10 секунд — режим настройки Wi-Fi
void checkBootButton() {
  static bool last = HIGH;
  static uint32_t changed = 0;
  bool now = digitalRead(BOOT_PIN);
  if (now != last && millis() - changed > 50) {  // защита от дребезга
    changed = millis();
    last = now;
    if (now == LOW) assistantTrigger();
  }
  if (last == LOW && millis() - changed > 10000) wifiRequestSetup();
}

void setupAssistant() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  // 16 КБ: в этой задаче идёт и живой разговор с Gemini (WebSocket, разбор больших JSON)
  xTaskCreatePinnedToCore(assistantTaskFn, "assistant", 16384, nullptr, 1, &asTask, 1);
}
