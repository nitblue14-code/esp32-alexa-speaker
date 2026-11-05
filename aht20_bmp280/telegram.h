// Telegram-бот: управлять колонкой из любого места, даже когда она в чужой сети Wi-Fi.
// Колонка сама спрашивает у Telegram новые сообщения (long polling, раз в 25 с) — роутер настраивать не нужно.
// Слушается только привязанных: код привязки — на странице колонки (⚙ Настройки → Telegram).
// Прошивка обновляется так: отправь боту файл kolonka.bin.
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <vector>
#include <utility>

#define TG_API        "https://api.telegram.org/bot"
#define TG_POLL_S     25    // сколько Telegram держит запрос, если новых сообщений нет
#define TG_MAX_CHATS  5     // сколько человек можно привязать

String tgToken;                       // токен бота от @BotFather — задаётся на странице
String tgBotName;                     // @имя бота, узнаём у Telegram
char tgStatus[120] = "бот не настроен";
char tgCode[8] = "";                  // код привязки, меняется после каждого использования
JsonDocument tgChats;                 // привязанные: [{"id":123,"n":"Имя"}]
SemaphoreHandle_t tgMux = nullptr;    // tgChats и очередь уведомлений трогают и сайт, и задача бота
std::vector<String> tgOutbox;         // уведомления, которые бот разошлёт всем привязанным
volatile bool tgReload = false;       // токен поменяли — начать заново

void tgLock() { xSemaphoreTake(tgMux, portMAX_DELAY); }
void tgUnlock() { xSemaphoreGive(tgMux); }

void tgNewCode() {
  snprintf(tgCode, sizeof(tgCode), "%06lu", (unsigned long)(esp_random() % 1000000));
}

void tgSaveChats() {
  String s;
  serializeJson(tgChats, s);
  prefs.putString("tgchats", s);
}

void tgLoad() {
  tgToken = prefs.getString("tgtoken", "");
  if (deserializeJson(tgChats, prefs.getString("tgchats", "[]")) || !tgChats.is<JsonArray>()) tgChats.to<JsonArray>();
  tgNewCode();
}

void tgSetToken(const String &t) {
  tgToken = t;
  tgToken.trim();
  prefs.putString("tgtoken", tgToken);
  tgReload = true;
}

bool tgAllowed(int64_t id) {
  tgLock();
  bool ok = false;
  for (JsonObject c : tgChats.as<JsonArray>()) ok = ok || c["id"].as<int64_t>() == id;
  tgUnlock();
  return ok;
}

void tgUnlink(int64_t id) {
  tgLock();
  JsonArray a = tgChats.as<JsonArray>();
  for (size_t i = 0; i < a.size(); i++)
    if (a[i]["id"].as<int64_t>() == id) {
      a.remove(i);
      break;
    }
  tgSaveChats();
  tgUnlock();
}

// Уведомление всем привязанным (можно звать из любой задачи)
void tgNotify(const String &text) {
  if (!tgMux || tgToken.isEmpty()) return;
  tgLock();
  if (tgOutbox.size() < 10) tgOutbox.push_back(text);
  tgUnlock();
}

// ---------- Запросы к Telegram ----------

int tgCall(const char *method, const String &body, String &resp, uint16_t timeoutMs = 15000) {
  WiFiClientSecure client;
  client.setInsecure();  // без проверки сертификата — как и для остальных сервисов
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, String(TG_API) + tgToken + "/" + method)) return -1;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  resp = code > 0 ? http.getString() : "";
  http.end();
  return code;
}

// Telegram не принимает текст с разрезанной посередине русской буквой — режем по границе символа
String tgTail(const String &s, size_t max) {
  if (s.length() <= max) return s;
  size_t from = s.length() - max;
  while (from < s.length() && ((uint8_t)s[from] & 0xC0) == 0x80) from++;
  return s.substring(from);
}

void tgDelete(int64_t chat, int32_t msgId) {
  JsonDocument d;
  d["chat_id"] = chat;
  d["message_id"] = msgId;
  String body, resp;
  serializeJson(d, body);
  tgCall("deleteMessage", body, resp);
}

// Отметить сообщения до offset прочитанными — перед перезагрузкой, иначе после неё та же команда придёт снова
void tgAck(int64_t offset) {
  JsonDocument d;
  d["offset"] = offset;
  d["timeout"] = 0;
  String body, resp;
  serializeJson(d, body);
  tgCall("getUpdates", body, resp);
}

// ---------- Кнопки ----------
// Внизу чата всегда висит клавиатура с главными кнопками. Нажатие — обычное сообщение с текстом кнопки.
// Под сообщениями бота — встроенные кнопки (панели музыки, громкости и т. д.); их нажатие приходит
// как callback_query, а панель обновляется на месте, без новых сообщений.

#define TG_B_STATUS "📊 Статус"
#define TG_B_MUSIC  "🎵 Музыка"
#define TG_B_VOL    "🔊 Громкость"
#define TG_B_NIGHT  "🌙 Ночь"
#define TG_B_SAY    "💬 Сказать"
#define TG_B_ASK    "❓ Спросить"
#define TG_B_MORE   "⚙ Ещё"

const char *TG_MAIN_KB =
  "{\"keyboard\":[[{\"text\":\"" TG_B_STATUS "\"},{\"text\":\"" TG_B_MUSIC "\"}],"
  "[{\"text\":\"" TG_B_VOL "\"},{\"text\":\"" TG_B_NIGHT "\"}],"
  "[{\"text\":\"" TG_B_SAY "\"},{\"text\":\"" TG_B_ASK "\"}],[{\"text\":\"" TG_B_MORE "\"}]],"
  "\"resize_keyboard\":true,\"is_persistent\":true}";

const char *TG_HELP =
  "Управляй кнопками внизу 👇\n"
  "📊 Статус — как дела у колонки\n"
  "🎵 Музыка — радио, Spotify, стоп\n"
  "🔊 Громкость — музыки и голоса\n"
  "🌙 Ночь — ночной режим\n"
  "💬 Сказать — колонка скажет твой текст голосом\n"
  "❓ Спросить — вопрос ассистенту\n"
  "⚙ Ещё — журнал, ключи, перезагрузка, прошивка\n\n"
  "Просто напиши вопрос — ассистент ответит. Чтобы обновить прошивку, пришли сюда файл kolonka.bin.";

// Встроенные кнопки: rows — ряды, в каждом пары {текст, данные}
struct TgBtn {
  String text, data;
};
typedef std::vector<std::vector<TgBtn>> TgRows;

String tgInline(const TgRows &rows) {
  JsonDocument d;
  JsonArray kb = d["inline_keyboard"].to<JsonArray>();
  for (auto &row : rows) {
    if (!row.size()) continue;
    JsonArray r = kb.add<JsonArray>();
    for (auto &b : row) {
      JsonObject o = r.add<JsonObject>();
      o["text"] = b.text;
      o["callback_data"] = b.data;
    }
  }
  String s;
  serializeJson(d, s);
  return s;
}

void tgSend(int64_t chat, const String &text, const String &markup = "") {
  JsonDocument d;
  d["chat_id"] = chat;
  d["text"] = tgTail(text, 4000);
  if (!markup.isEmpty()) d["reply_markup"] = serialized(markup);
  String body, resp;
  serializeJson(d, body);
  int code = tgCall("sendMessage", body, resp);
  if (code != 200) Serial.printf("[telegram] не отправилось (%d): %s\n", code, resp.substring(0, 200).c_str());
}

// Обновить панель на месте (то же сообщение)
void tgEdit(int64_t chat, int32_t msgId, const String &text, const String &markup = "") {
  JsonDocument d;
  d["chat_id"] = chat;
  d["message_id"] = msgId;
  d["text"] = tgTail(text, 4000);
  if (!markup.isEmpty()) d["reply_markup"] = serialized(markup);
  String body, resp;
  serializeJson(d, body);
  tgCall("editMessageText", body, resp);  // «текст не изменился» — не ошибка, молча пропускаем
}

// Убрать «часики» с нажатой кнопки (и показать короткую всплывашку)
void tgAnswerCb(const String &id, const String &text = "") {
  JsonDocument d;
  d["callback_query_id"] = id;
  if (!text.isEmpty()) d["text"] = text;
  String body, resp;
  serializeJson(d, body);
  tgCall("answerCallbackQuery", body, resp);
}

// ---------- Панели ----------

String tgStatusText() {
  String s = "🔊 Колонка на связи\n";
  uint32_t up = millis() / 1000;
  s += "Работает: " + String(up / 86400) + " д " + String(up % 86400 / 3600) + " ч " + String(up % 3600 / 60) + " мин\n";
  s += "Wi-Fi: " + wifiSsid + " (" + String(WiFi.RSSI()) + " dBm), IP " + WiFi.localIP().toString() + "\n";
  s += "В комнате: " + String(curTemp, 1) + " °C, " + String(lroundf(curHum)) + " %, " +
       String(lroundf(curPressure * 0.750062f)) + " мм рт. ст.\n";
  String air = airStatus();
  if (!air.isEmpty()) s += "⚠ " + air + "\n";
  s += "Музыка: " + String(radioUrl[0] ? radioName : "выключена");
  if (radioUrl[0] && nowTitle[0]) s += String(" — ") + nowTitle;
  s += "\nГромкость: музыка " + String(volume) + ", голос " + String(voiceVolume) + " из 21\n";
  if (nightActive()) s += "🌙 Ночной режим до " + formatClock(nightUntil, false) + "\n";
  s += "Будильники и таймеры: " + eventsText() + "\n";
  s += String("Ключи: Groq ") + (groqKey.isEmpty() ? "—" : "✓") + ", Gemini " + (liveKey.isEmpty() ? "—" : "✓") +
       ", Google " + (gcloudKey.isEmpty() ? "—" : "✓") + ", Spotify " + (spotifyReady() ? "✓" : "—") + "\n";
  if (asError[0]) s += String("Последняя ошибка: ") + asError + "\n";
  s += "Память: " + String(ESP.getFreeHeap() / 1024) + " КБ свободно";
  return s;
}

String tgStatusKb() { return tgInline({{{"🔄 Обновить", "status"}}}); }

String tgMusicText() {
  String s = "🎵 Музыка\nСейчас: " + String(radioUrl[0] ? radioName : "ничего не играет");
  if (radioUrl[0] && nowTitle[0]) s += String(" — ") + nowTitle;
  return s;
}

String tgMusicKb() {
  TgRows rows;
  std::vector<TgBtn> row;
  for (size_t i = 0; i < STATION_COUNT; i++) {  // станции по две в ряд
    row.push_back({STATIONS[i].name, String("st:") + STATIONS[i].id});
    if (row.size() == 2) {
      rows.push_back(row);
      row.clear();
    }
  }
  row.push_back({"■ Стоп", "stop"});
  rows.push_back(row);
  rows.push_back({{"🔍 Найти станцию", "find"}});
  if (spotifyReady()) rows.push_back({{"🎧 Spotify: включить…", "sp"}, {"⏭ Следующий", "sp:next"}});
  return tgInline(rows);
}

String tgVolText() {
  return "🔊 Громкость (из 21)\nМузыка: " + String(volume) + "\nГолос: " + String(voiceVolume);
}

String tgVolKb() {
  return tgInline({
    {{"🎵 −", "v:m-"}, {"🎵 " + String(volume), "v:m"}, {"🎵 +", "v:m+"}},
    {{"🗣 −", "v:v-"}, {"🗣 " + String(voiceVolume), "v:v"}, {"🗣 +", "v:v+"}},
  });
}

String tgNightText() {
  return nightActive() ? "🌙 Ночной режим включён до " + formatClock(nightUntil, false) +
                             ".\nСветодиод не горит, голос тише, без предупреждений о воздухе."
                       : String("☀ Ночной режим выключен.");
}

String tgNightKb() {
  return nightActive() ? tgInline({{{"☀ Выключить", "night:off"}}})
                       : tgInline({{{"🌙 Включить (до 7:00)", "night:on"}}, {{"🌙 + выключить музыку через 30 мин", "night:30"}}});
}

String tgMoreKb() {
  return tgInline({
    {{"📜 Журнал", "log"}, {"🔑 Ключи", "keys"}},
    {{"⬆ Обновить прошивку", "fw"}, {"🔄 Перезагрузить", "restart"}},
    {{"🔓 Отвязать этот чат", "unlink"}},
  });
}

// ---------- Ожидание текста после кнопки ----------
// Нажали «Сказать» — следующее сообщение из этого чата и есть текст, который надо сказать.

enum TgWait : uint8_t { TGW_NONE, TGW_SAY, TGW_ASK, TGW_FIND, TGW_SPOTIFY, TGW_KEY_GROQ, TGW_KEY_GEMINI, TGW_KEY_GOOGLE };
std::vector<std::pair<int64_t, TgWait>> tgWaiting;

void tgSetWait(int64_t chat, TgWait w) {
  for (auto &p : tgWaiting)
    if (p.first == chat) {
      p.second = w;
      return;
    }
  tgWaiting.push_back({chat, w});
}

TgWait tgTakeWait(int64_t chat) {
  for (auto &p : tgWaiting)
    if (p.first == chat) {
      TgWait w = p.second;
      p.second = TGW_NONE;
      return w;
    }
  return TGW_NONE;
}

const String TG_CANCEL_KB = "{\"inline_keyboard\":[[{\"text\":\"✕ Отмена\",\"callback_data\":\"cancel\"}]]}";

// ---------- Действия ----------

// Спросить ассистента так же, как с сайта, и дождаться ответа (он ещё и прозвучит дома)
String tgAsk(const String &q) {
  asAnswer[0] = 0;
  asError[0] = 0;
  assistantAsk(q);
  uint32_t t0 = millis();
  while (millis() - t0 < 90000) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (asHasTyped) continue;  // ещё не взялся
    if (asAnswer[0] && asState == AS_IDLE) return asAnswer;
    if (asError[0]) return String("Ошибка: ") + asError;
  }
  return asAnswer[0] ? String(asAnswer) : String("Не дождался ответа.");
}

String tgPlay(String arg) {
  arg.trim();
  if (arg.isEmpty()) arg = "hitfm";
  for (size_t i = 0; i < STATION_COUNT; i++)
    if (arg.equalsIgnoreCase(STATIONS[i].id) || arg.equalsIgnoreCase(STATIONS[i].name)) {
      radioPlay(STATIONS[i].url, STATIONS[i].name);
      return String("▶ ") + STATIONS[i].name;
    }
  if (radioSearch("", arg, "", "") == 0 && radioSearch(arg, "", "", "") == 0) return "Не нашёл такую станцию.";
  radioPlay(radioFound[0].url, radioFound[0].name);
  return "▶ " + radioFound[0].name;
}

String tgSpotify(const String &query) {
  JsonDocument act;
  act["do"] = "spotify_play";
  act["query"] = query;
  act["type"] = "track";
  asActionInfo = "";
  String failed = asRunAction(act.as<JsonObject>());
  if (failed.isEmpty() && radioUrl[0]) sendAudioCmd(AUDIO_PLAY, radioUrl);
  return failed.isEmpty() ? "▶ " + (asActionInfo.isEmpty() ? query : asActionInfo) : failed;
}

String tgSetKey(const char *which, const String &key) {
  String k = key;
  k.trim();
  if (k.length() < 10) return "Это не похоже на ключ.";
  if (!strcmp(which, "groq")) {
    groqKey = k;
    prefs.putString("groq", k);
  } else if (!strcmp(which, "gemini")) {
    liveKey = k;
    prefs.putString("gmkey", k);
  } else {
    gcloudKey = k;
    prefs.putString("gckey", k);
  }
  return String("🔑 Ключ ") + which + " сохранён, сообщение с ним удалила.";
}

// Прошивка из файла, присланного боту
bool tgFirmware(const String &fileId, String &err) {
  JsonDocument d;
  d["file_id"] = fileId;
  String body, resp;
  serializeJson(d, body);
  if (tgCall("getFile", body, resp) != 200) {
    err = "Telegram не отдал файл (больше 20 МБ?)";
    return false;
  }
  JsonDocument f;
  deserializeJson(f, resp);
  String path = f["result"]["file_path"] | "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, "https://api.telegram.org/file/bot" + tgToken + "/" + path)) {
    err = "не удалось начать загрузку";
    return false;
  }
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len < 100000) {
    err = "загрузка не удалась (" + String(code) + ")";
    http.end();
    return false;
  }
  sendAudioCmd(AUDIO_STOP);  // чтобы звук не мешал записи
  Serial.printf("[telegram] прошивка из Telegram: %d байт\n", len);
  bool ok = Update.begin(len) && Update.writeStream(*http.getStreamPtr()) == (size_t)len && Update.end(true);
  if (!ok) {
    err = Update.errorString();
    Update.abort();
  }
  http.end();
  return ok;
}

void tgRestart(int64_t nextOffset) {
  tgAck(nextOffset);
  delay(500);
  ESP.restart();
}

// ---------- Сообщения ----------

// Чужой чат: слушаемся только после кода привязки с сайта колонки
void tgLinkAttempt(JsonObject msg, int64_t chat, const String &text) {
  static uint8_t wrong = 0;
  static uint32_t wrongAt = 0;
  String code = text.startsWith("/start") ? text.substring(6) : text;
  code.trim();
  if (wrong >= 5 && millis() - wrongAt < 60000) return;  // подбор кода — минуту молчим
  if (code.length() == 6 && code == tgCode) {
    tgLock();
    bool full = tgChats.size() >= TG_MAX_CHATS;
    if (!full) {
      JsonObject c = tgChats.as<JsonArray>().add<JsonObject>();
      c["id"] = chat;
      String name = msg["from"]["first_name"] | "";
      if (msg["from"]["username"].is<const char *>()) name += String(" @") + (msg["from"]["username"] | "");
      c["n"] = name;
      tgSaveChats();
    }
    tgUnlock();
    tgNewCode();
    wrong = 0;
    if (full) tgSend(chat, "Привязано уже " + String(TG_MAX_CHATS) + " чатов — отвяжи кого-нибудь на странице колонки.");
    else tgSend(chat, String("✅ Привязано! Теперь колонка слушается тебя.\n\n") + TG_HELP, TG_MAIN_KB);
    Serial.printf("[telegram] привязан чат %lld\n", (long long)chat);
    return;
  }
  if (code.length() == 6) {
    wrong++;
    wrongAt = millis();
  }
  tgSend(chat, "Это бот колонки Alexa. Чтобы управлять ей, пришли сюда код привязки — "
               "он на странице колонки: ⚙ Настройки → Telegram.");
}

void tgHandle(JsonObject msg, int64_t nextOffset) {
  int64_t chat = msg["chat"]["id"];
  int32_t msgId = msg["message_id"];
  String text = msg["text"] | "";
  text.trim();

  if (!tgAllowed(chat)) return tgLinkAttempt(msg, chat, text);

  // Файл прошивки
  if (msg["document"].is<JsonObject>()) {
    String name = msg["document"]["file_name"] | "";
    if (!name.endsWith(".bin")) return tgSend(chat, "Это не прошивка — нужен файл .bin.");
    tgSend(chat, "⬆ Обновляю прошивку…");
    String err;
    if (tgFirmware(msg["document"]["file_id"] | "", err)) {
      tgSend(chat, "✅ Прошивка записана, перезагружаюсь. Через минуту напишу, что включилась.");
      tgRestart(nextOffset);
    }
    return tgSend(chat, "❌ Не получилось: " + err);
  }
  if (text.isEmpty()) return;

  // Кнопки главной клавиатуры (нажали другую кнопку — то, что ждали, отменяется)
  bool mainBtn = text == TG_B_STATUS || text == TG_B_MUSIC || text == TG_B_VOL || text == TG_B_NIGHT ||
                 text == TG_B_MORE || text == TG_B_SAY || text == TG_B_ASK;
  if (mainBtn) tgTakeWait(chat);
  if (text == TG_B_STATUS) return tgSend(chat, tgStatusText(), tgStatusKb());
  if (text == TG_B_MUSIC) return tgSend(chat, tgMusicText(), tgMusicKb());
  if (text == TG_B_VOL) return tgSend(chat, tgVolText(), tgVolKb());
  if (text == TG_B_NIGHT) return tgSend(chat, tgNightText(), tgNightKb());
  if (text == TG_B_MORE) return tgSend(chat, "⚙ Ещё", tgMoreKb());
  if (text == TG_B_SAY) {
    tgSetWait(chat, TGW_SAY);
    return tgSend(chat, "💬 Напиши, что сказать — колонка произнесёт это голосом.", TG_CANCEL_KB);
  }
  if (text == TG_B_ASK) {
    tgSetWait(chat, TGW_ASK);
    return tgSend(chat, "❓ Напиши вопрос — ассистент ответит голосом дома и сюда.", TG_CANCEL_KB);
  }

  // Ждали текст после кнопки
  TgWait wait = tgTakeWait(chat);
  switch (wait) {
    case TGW_SAY:
      strlcpy(asAnswer, text.c_str(), sizeof(asAnswer));
      sendAudioCmd(AUDIO_SAY, text);
      return tgSend(chat, "🔊 Сказала.");
    case TGW_ASK:
      tgSend(chat, "🤔 Думаю…");
      return tgSend(chat, tgAsk(text));
    case TGW_FIND:
      return tgSend(chat, tgPlay(text), tgMusicKb());
    case TGW_SPOTIFY:
      return tgSend(chat, tgSpotify(text), tgMusicKb());
    case TGW_KEY_GROQ:
    case TGW_KEY_GEMINI:
    case TGW_KEY_GOOGLE:
      tgDelete(chat, msgId);  // ключ не должен висеть в переписке
      return tgSend(chat, tgSetKey(wait == TGW_KEY_GROQ ? "groq" : wait == TGW_KEY_GEMINI ? "gemini" : "google", text));
    default:
      break;
  }

  // Старые команды тоже работают
  int sp = text.indexOf(' ');
  String cmd = sp < 0 ? text : text.substring(0, sp);
  String arg = sp < 0 ? String("") : text.substring(sp + 1);
  arg.trim();
  int at = cmd.indexOf('@');  // «/status@ИмяБота» в группах
  if (at > 0) cmd = cmd.substring(0, at);
  cmd.toLowerCase();

  if (cmd == "/start" || cmd == "/help") return tgSend(chat, TG_HELP, TG_MAIN_KB);
  if (cmd == "/status") return tgSend(chat, tgStatusText(), tgStatusKb());
  if (cmd == "/say" && !arg.isEmpty()) {
    sendAudioCmd(AUDIO_SAY, arg);
    return tgSend(chat, "🔊 Сказала.");
  }
  if (cmd == "/play") return tgSend(chat, tgPlay(arg), tgMusicKb());
  if (cmd == "/stop") {
    asStopMusic();
    sendAudioCmd(AUDIO_STOP);
    return tgSend(chat, "■ Выключила.");
  }
  if (cmd == "/log") return tgSend(chat, tgTail(webLog.text(), 3800));
  if (cmd == "/restart") {
    tgSend(chat, "🔄 Перезагружаюсь.");
    return tgRestart(nextOffset);
  }
  if (cmd.startsWith("/")) return tgSend(chat, TG_HELP, TG_MAIN_KB);

  // Просто текст — вопрос ассистенту
  tgSend(chat, "🤔 Думаю…");
  tgSend(chat, tgAsk(text));
}

// Нажатие встроенной кнопки под сообщением бота
void tgHandleCb(JsonObject cb, int64_t nextOffset) {
  String id = cb["id"] | "";
  String data = cb["data"] | "";
  int64_t chat = cb["message"]["chat"]["id"];
  int32_t msgId = cb["message"]["message_id"];
  if (!tgAllowed(chat)) return tgAnswerCb(id, "Чат не привязан");
  Serial.printf("[telegram] кнопка %s\n", data.c_str());

  if (data == "status") {
    tgAnswerCb(id);
    return tgEdit(chat, msgId, tgStatusText(), tgStatusKb());
  }
  if (data.startsWith("st:")) {
    String r = tgPlay(data.substring(3));
    tgAnswerCb(id, r);
    vTaskDelay(pdMS_TO_TICKS(1500));  // пусть станция подключится — в панели будет название
    return tgEdit(chat, msgId, tgMusicText(), tgMusicKb());
  }
  if (data == "stop") {
    asStopMusic();
    sendAudioCmd(AUDIO_STOP);
    tgAnswerCb(id, "■ Выключила");
    return tgEdit(chat, msgId, tgMusicText(), tgMusicKb());
  }
  if (data == "find") {
    tgAnswerCb(id);
    tgSetWait(chat, TGW_FIND);
    return tgSend(chat, "🔍 Напиши жанр или название: jazz, rock, lofi, Radio Record…", TG_CANCEL_KB);
  }
  if (data == "sp") {
    tgAnswerCb(id);
    tgSetWait(chat, TGW_SPOTIFY);
    return tgSend(chat, "🎧 Что включить в Spotify? Песня и исполнитель, например: Обійми Океан Ельзи", TG_CANCEL_KB);
  }
  if (data == "sp:next") {
    String failed = spotifyControl("next");
    tgAnswerCb(id, failed.isEmpty() ? "⏭ Следующий трек" : failed);
    return;
  }
  if (data.startsWith("v:")) {
    bool music = data[2] == 'm';
    int cur = music ? volume : voiceVolume;
    int v = constrain(cur + (data.endsWith("+") ? 1 : data.endsWith("-") ? -1 : 0), 0, 21);
    if (music) {
      audioSetVolume(v);
      prefs.putUChar("vol", volume);
    } else {
      audioSetVoiceVolume(v);
      prefs.putUChar("vvol", voiceVolume);
    }
    tgAnswerCb(id, (music ? "Музыка " : "Голос ") + String(v));
    return tgEdit(chat, msgId, tgVolText(), tgVolKb());
  }
  if (data.startsWith("night:")) {
    if (data == "night:off") nightEnd();
    else {
      nightStart();
      if (data == "night:30" && radioUrl[0]) sleepStopAt = time(nullptr) + 30 * 60;
    }
    tgAnswerCb(id);
    return tgEdit(chat, msgId, tgNightText(), tgNightKb());
  }
  if (data == "log") {
    tgAnswerCb(id);
    return tgSend(chat, tgTail(webLog.text(), 3800));
  }
  if (data == "keys") {
    tgAnswerCb(id);
    return tgSend(chat, "🔑 Какой ключ поменять? Пришлёшь его следующим сообщением — я сразу его удалю из чата.",
                  tgInline({{{"Groq", "key:groq"}, {"Gemini", "key:gemini"}, {"Google Cloud", "key:google"}},
                            {{"✕ Отмена", "cancel"}}}));
  }
  if (data.startsWith("key:")) {
    String w = data.substring(4);
    tgSetWait(chat, w == "groq" ? TGW_KEY_GROQ : w == "gemini" ? TGW_KEY_GEMINI : TGW_KEY_GOOGLE);
    tgAnswerCb(id);
    return tgEdit(chat, msgId, "🔑 Пришли ключ " + w + " следующим сообщением.", TG_CANCEL_KB);
  }
  if (data == "fw") {
    tgAnswerCb(id);
    return tgSend(chat, "⬆ Пришли сюда файл kolonka.bin (как документ) — колонка сама прошьётся и перезагрузится.");
  }
  if (data == "restart") {
    tgAnswerCb(id);
    return tgEdit(chat, msgId, "🔄 Точно перезагрузить колонку?",
                  tgInline({{{"Да, перезагрузить", "restart!"}, {"✕ Отмена", "cancel"}}}));
  }
  if (data == "restart!") {
    tgAnswerCb(id, "Перезагружаюсь");
    tgEdit(chat, msgId, "🔄 Перезагружаюсь. Через минуту напишу, что включилась.");
    return tgRestart(nextOffset);
  }
  if (data == "unlink") {
    tgAnswerCb(id);
    return tgEdit(chat, msgId, "🔓 Отвязать этот чат? Колонка перестанет слушаться команд отсюда.",
                  tgInline({{{"Да, отвязать", "unlink!"}, {"✕ Отмена", "cancel"}}}));
  }
  if (data == "unlink!") {
    tgUnlink(chat);
    tgAnswerCb(id, "Отвязано");
    return tgEdit(chat, msgId, "Чат отвязан. Чтобы снова управлять колонкой — пришли новый код привязки с её страницы.");
  }
  if (data == "cancel") {
    tgTakeWait(chat);
    tgAnswerCb(id, "Отменено");
    return tgEdit(chat, msgId, "Отменено.");
  }
  tgAnswerCb(id);
}

void tgSetCommands() {
  static const char *cmds[][2] = {{"start", "Показать кнопки"}, {"status", "Как дела у колонки"}};
  JsonDocument d;
  for (auto &c : cmds) {
    JsonObject o = d["commands"].add<JsonObject>();
    o["command"] = c[0];
    o["description"] = c[1];
  }
  String body, resp;
  serializeJson(d, body);
  tgCall("setMyCommands", body, resp);
}

void tgTask(void *) {
  int64_t offset = 0;
  bool hello = false;
  for (;;) {
    if (tgReload) {
      tgReload = false;
      hello = false;
      offset = 0;
    }
    if (tgToken.isEmpty()) {
      strlcpy(tgStatus, "бот не настроен", sizeof(tgStatus));
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED || wifiSetupActive) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    String resp;
    if (!hello) {  // проверяем токен, ставим меню команд, сообщаем привязанным, что колонка включилась
      int code = tgCall("getMe", "{}", resp);
      if (code != 200) {
        snprintf(tgStatus, sizeof(tgStatus), code == 401 || code == 404 ? "неверный токен" : "нет связи с Telegram (%d)", code);
        vTaskDelay(pdMS_TO_TICKS(code == 401 || code == 404 ? 60000 : 10000));
        continue;
      }
      JsonDocument me;
      deserializeJson(me, resp);
      tgBotName = String("@") + (me["result"]["username"] | "");
      snprintf(tgStatus, sizeof(tgStatus), "работает: %s", tgBotName.c_str());
      tgSetCommands();
      tgNotify("🔊 Колонка включилась. Wi-Fi " + wifiSsid + ", IP " + WiFi.localIP().toString() + ".");
      hello = true;
    }

    // Разослать накопившиеся уведомления
    tgLock();
    std::vector<String> out;
    out.swap(tgOutbox);
    std::vector<int64_t> ids;
    for (JsonObject c : tgChats.as<JsonArray>()) ids.push_back(c["id"].as<int64_t>());
    tgUnlock();
    for (const String &t : out)
      for (int64_t id : ids) tgSend(id, t);

    // Ждём новые сообщения (до 25 с)
    JsonDocument req;
    req["offset"] = offset;
    req["timeout"] = TG_POLL_S;
    req["allowed_updates"].add("message");
    req["allowed_updates"].add("callback_query");  // нажатия встроенных кнопок
    String body;
    serializeJson(req, body);
    int code = tgCall("getUpdates", body, resp, (TG_POLL_S + 10) * 1000);
    if (code != 200) {
      if (code == 401 || code == 404) {
        strlcpy(tgStatus, "неверный токен", sizeof(tgStatus));
        hello = false;
      }
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    JsonDocument upd;
    if (deserializeJson(upd, resp)) continue;
    for (JsonObject u : upd["result"].as<JsonArray>()) {
      offset = u["update_id"].as<int64_t>() + 1;
      if (u["message"].is<JsonObject>()) tgHandle(u["message"], offset);
      else if (u["callback_query"].is<JsonObject>()) tgHandleCb(u["callback_query"], offset);
    }
  }
}

void setupTelegram() {
  tgMux = xSemaphoreCreateMutex();
  tgLoad();
  // 16 КБ: защищённое соединение и разбор JSON; ядро 0, низкий приоритет — звуку не мешает
  xTaskCreatePinnedToCore(tgTask, "telegram", 16384, nullptr, 1, nullptr, 0);
}
