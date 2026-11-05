// Синтез речи через Google Cloud Text-to-Speech, голоса Chirp 3 HD (Leda, Kore, Aoede…).
// Бесплатно 1 млн символов в месяц. Чтобы точно не платить, считаем символы сами и после
// GCLOUD_MONTH_LIMIT до конца месяца говорим запасным голосом Google Translate.
// Ключ вводится на веб-странице. Если ключа нет или сервис не ответил — тоже запасной голос.
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <time.h>

#define GCLOUD_URL         "https://texttospeech.googleapis.com/v1/text:synthesize"
// Звук заказываем в Ogg Opus 48 кГц: маленький (~30–40 КБ на фразу, синтез ~1 с) и без пересчёта частоты.
// Несжатый WAV пробовали — скачивался в ~10 раз дольше, а треск давала не сжатие, а запись во флеш (её больше нет).
#define GCLOUD_TIMEOUT_MS  10000
#define GCLOUD_MONTH_LIMIT 900000UL  // из бесплатного 1 000 000 — с запасом

extern Preferences prefs;

String gcloudKey;                  // ключ API из Google Cloud Console
char ttsStatus[120] = "";          // как прошёл последний синтез — показываем на странице
uint32_t gcloudMonthChars = 0;     // сколько символов потрачено в этом месяце
uint32_t gcloudMonth = 0;          // какой это месяц: ГГГГММ
// Google отдаёт голос почти на пределе: уже с 0 дБ пики обрезаются и голос хрипит. Берём с запасом,
// а громкость и усиление делаем на плате (audio_player.h) — там пики сжимаются мягко.
const float gcloudGainDb = -4.0;

void gcloudLoadUsage() {
  gcloudMonth = prefs.getULong("gcmonth", 0);
  gcloudMonthChars = prefs.getULong("gcchars", 0);
}

uint32_t gcloudCurrentMonth() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;  // часы ещё не синхронизированы
  struct tm t;
  localtime_r(&now, &t);
  return (t.tm_year + 1900) * 100 + t.tm_mon + 1;
}

// Сколько символов в UTF-8 строке (Google считает символы, а не байты)
uint32_t utf8Length(const char *s) {
  uint32_t n = 0;
  for (; *s; s++)
    if ((*s & 0xC0) != 0x80) n++;
  return n;
}

// Хватит ли бесплатного лимита на этот текст; заодно обнуляет счётчик в новом месяце
bool gcloudHasQuota(uint32_t chars) {
  uint32_t m = gcloudCurrentMonth();
  if (m && m != gcloudMonth) {  // новый месяц — бывает раз в месяц, можно записать сразу
    gcloudMonth = m;
    gcloudMonthChars = 0;
    prefs.putULong("gcmonth", gcloudMonth);
    prefs.putULong("gcchars", 0);
  }
  return gcloudMonthChars + chars <= GCLOUD_MONTH_LIMIT;
}

String gcloudJsonEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

// Соединение с Google держим открытым, пока говорится одна фраза (все её куски идут по нему),
// и закрываем после — TLS-буферы занимают много внутренней памяти.
WiFiClientSecure gcloudClient;
HTTPClient gcloudHttp;
bool gcloudClientReady = false;

void gcloudClose() {
  gcloudClient.stop();
}

// Один запрос с повтором: если открытое соединение успело «протухнуть», второй раз подключаемся заново
int gcloudPost(const String &key, const String &body, String &resp) {
  if (!gcloudClientReady) {
    gcloudClient.setInsecure();
    gcloudClientReady = true;
  }
  int code = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    gcloudHttp.setReuse(true);
    gcloudHttp.begin(gcloudClient, GCLOUD_URL);
    gcloudHttp.setTimeout(GCLOUD_TIMEOUT_MS);
    gcloudHttp.addHeader("Content-Type", "application/json; charset=utf-8");
    gcloudHttp.addHeader("X-Goog-Api-Key", key);
    code = gcloudHttp.POST(body);
    if (code > 0) {
      resp = gcloudHttp.getString();
      gcloudHttp.end();
      return code;
    }
    resp = HTTPClient::errorToString(code);
    gcloudHttp.end();
    gcloudClient.stop();
  }
  return code;
}

// Счётчик символов сохраняем во флеш не сразу, а когда колонка договорит: запись во флеш
// на доли секунды останавливает процессор, и звук в этот момент трещит
bool gcloudUsageDirty = false;

void gcloudSaveUsage() {
  if (!gcloudUsageDirty) return;
  gcloudUsageDirty = false;
  prefs.putULong("gcchars", gcloudMonthChars);
}

// Говорит text голосом voice (например "ru-RU-Chirp3-HD-Leda"). Звук (Ogg Opus) — в out/outLen,
// в PSRAM; освободить free(). Во флеш ничего не пишем — см. выше. true — звук готов.
bool gcloudTtsToMem(const String &text, const char *voice, uint8_t *&out, size_t &outLen) {
  String key = gcloudKey;
  if (key.isEmpty()) return false;

  uint32_t chars = utf8Length(text.c_str());
  if (!gcloudHasQuota(chars)) {
    snprintf(ttsStatus, sizeof(ttsStatus), "бесплатный лимит месяца исчерпан — до 1-го числа голос попроще");
    return false;
  }

  String body = String("{\"input\":{\"text\":\"") + gcloudJsonEscape(text) +
                "\"},\"voice\":{\"languageCode\":\"ru-RU\",\"name\":\"" + voice +
                "\"},\"audioConfig\":{\"audioEncoding\":\"OGG_OPUS\",\"sampleRateHertz\":48000,"
                "\"volumeGainDb\":" + String(gcloudGainDb, 1) + "}}";

  uint32_t t0 = millis();
  String resp;
  int code = gcloudPost(key, body, resp);

  if (code != 200) {
    if (code == 400 || code == 403)
      snprintf(ttsStatus, sizeof(ttsStatus), "Google Cloud: ключ не подходит или API не включён (%d)", code);
    else if (code == 429)
      snprintf(ttsStatus, sizeof(ttsStatus), "Google Cloud: слишком много запросов (429)");
    else
      snprintf(ttsStatus, sizeof(ttsStatus), "Google Cloud: ошибка %d", code);
    Serial.printf("[tts] %s: %s\n", ttsStatus, resp.substring(0, 200).c_str());
    return false;
  }

  // Ответ: {"audioContent": "<звук в base64>"} — достаём строку и раскодируем
  int start = resp.indexOf("\"audioContent\"");
  start = start < 0 ? -1 : resp.indexOf('"', resp.indexOf(':', start) + 1);
  int end = start < 0 ? -1 : resp.indexOf('"', start + 1);
  if (start < 0 || end < 0) {
    snprintf(ttsStatus, sizeof(ttsStatus), "Google Cloud: в ответе нет звука");
    return false;
  }
  const unsigned char *b64 = (const unsigned char *)resp.c_str() + start + 1;
  size_t b64Len = end - start - 1;
  size_t mp3Cap = b64Len * 3 / 4 + 4, mp3Len = 0;
  uint8_t *mp3 = (uint8_t *)ps_malloc(mp3Cap);
  if (!mp3 || mbedtls_base64_decode(mp3, mp3Cap, &mp3Len, b64, b64Len) != 0) {
    free(mp3);
    snprintf(ttsStatus, sizeof(ttsStatus), "Google Cloud: не удалось раскодировать звук");
    return false;
  }

  out = mp3;
  outLen = mp3Len;

  gcloudMonthChars += chars;
  gcloudUsageDirty = true;
  snprintf(ttsStatus, sizeof(ttsStatus), "Google Cloud: OK за %lu мс", (unsigned long)(millis() - t0));
  Serial.printf("[tts] %s, %u байт, за месяц %lu символов, свободно внутр. памяти %u\n", ttsStatus,
                (unsigned)mp3Len, (unsigned long)gcloudMonthChars,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}
