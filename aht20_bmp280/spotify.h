// Spotify: голосом включаем песни, исполнителей, альбомы и плейлисты на устройстве Spotify пользователя
// (телефон, компьютер, ТВ — где открыт Spotify). Через официальный Web API; нужен Spotify Premium.
//
// Настройка — один раз на странице колонки:
//  1. На developer.spotify.com создать приложение, Redirect URI: http://127.0.0.1:8888/callback,
//     вписать на странице Client ID и Client Secret.
//  2. Нажать «Войти в Spotify», разрешить доступ. Браузер перейдёт на 127.0.0.1 и покажет ошибку — это
//     нормально: адрес из строки браузера (с ?code=…) вставить на странице колонки. Spotify разрешает
//     http-адрес возврата только для 127.0.0.1, поэтому на саму колонку он вернуться не может.
//  3. Колонка получает refresh-токен и дальше сама обновляет доступ.
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/base64.h>

extern Preferences prefs;

#define SPOTIFY_REDIRECT "http://127.0.0.1:8888/callback"
#define SPOTIFY_SCOPES   "user-read-playback-state user-modify-playback-state user-read-currently-playing playlist-read-private"

String spClientId, spClientSecret, spRefresh, spDevice;
// Мост на ПК: там Spotify Soloist (устройство с именем spDevice) играет в виртуальный аудиокабель,
// а скрипт отдаёт этот звук потоком MP3 по этому адресу — колонка играет его, как радио.
// Пусто — моста нет, музыка играет на самом устройстве Spotify (телефон, ПК).
String spStreamUrl;
String spAccess;
uint32_t spAccessAt = 0, spAccessTtl = 0;  // когда получили токен доступа и сколько он живёт (мс)
char spStatus[160] = "не настроен";
String asActionInfo;  // что сделали — для ответа Gemini («включаю такую-то песню»)
// Просьба включить из живого разговора: выполняем после него, когда соединение с Gemini закрыто —
// два защищённых соединения сразу плохо уживаются в памяти, и разговор зависал на запросе к Spotify
String spPendingQuery, spPendingType;

bool spotifyReady() {
  return !spClientId.isEmpty() && !spClientSecret.isEmpty() && !spRefresh.isEmpty();
}

void spotifyLoad() {
  spClientId = prefs.getString("spid", "");
  spClientSecret = prefs.getString("spsec", "");
  spRefresh = prefs.getString("sprefresh", "");
  spDevice = prefs.getString("spdev", "");
  spStreamUrl = prefs.getString("spstream", "");
  if (!spStreamUrl.isEmpty() && spDevice.isEmpty()) spDevice = "Kolonka";  // имя по умолчанию в скрипте моста
  if (spotifyReady()) strlcpy(spStatus, "подключён", sizeof(spStatus));
  else if (!spClientId.isEmpty()) strlcpy(spStatus, "нужно войти в Spotify", sizeof(spStatus));
}

String spUrlEncode(const String &s) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

String spotifyAuthUrl() {
  return "https://accounts.spotify.com/authorize?response_type=code&client_id=" + spUrlEncode(spClientId) +
         "&redirect_uri=" + spUrlEncode(SPOTIFY_REDIRECT) + "&scope=" + spUrlEncode(SPOTIFY_SCOPES);
}

// Запрос токена (по коду после входа или по refresh-токену)
bool spTokenRequest(const String &form) {
  String cred = spClientId + ":" + spClientSecret;
  unsigned char b64[256];
  size_t n = 0;
  if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &n, (const unsigned char *)cred.c_str(), cred.length()) != 0) {
    strlcpy(spStatus, "слишком длинные Client ID/Secret", sizeof(spStatus));
    return false;
  }
  b64[n] = 0;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);  // секунд; по умолчанию 120 — столько можно ждать впустую
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", String("Basic ") + (char *)b64);
  uint32_t t0 = millis();
  Serial.printf("[spotify] обновляю доступ… (%u внутр. памяти)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  int code = http.POST(form);
  String resp = code > 0 ? http.getString() : String();
  http.end();
  Serial.printf("[spotify] доступ -> %d за %lu мс\n", code, (unsigned long)(millis() - t0));

  JsonDocument doc;
  deserializeJson(doc, resp);
  if (code != 200) {
    const char *why = doc["error_description"] | (const char *)nullptr;
    if (!why) why = doc["error"] | "нет связи";
    snprintf(spStatus, sizeof(spStatus), "Spotify не выдал доступ (%d): %s", code, why);
    Serial.printf("[spotify] %s\n", spStatus);
    return false;
  }
  spAccess = doc["access_token"] | "";
  spAccessTtl = (uint32_t)(doc["expires_in"] | 3600) * 1000;
  spAccessAt = millis();
  const char *rt = doc["refresh_token"] | (const char *)nullptr;
  if (rt && spRefresh != rt) {  // Spotify может выдать новый refresh-токен — сохраняем
    spRefresh = rt;
    prefs.putString("sprefresh", spRefresh);
  }
  strlcpy(spStatus, "подключён", sizeof(spStatus));
  return !spAccess.isEmpty();
}

// Вход: вставили адрес из браузера после разрешения доступа (или только код из него)
bool spotifyLoginWithCode(String pasted) {
  pasted.trim();
  String code = pasted;
  int i = pasted.indexOf("code=");
  if (i >= 0) {
    code = pasted.substring(i + 5);
    int amp = code.indexOf('&');
    if (amp >= 0) code = code.substring(0, amp);
  }
  if (code.isEmpty()) {
    strlcpy(spStatus, "в адресе нет кода — войди ещё раз", sizeof(spStatus));
    return false;
  }
  spRefresh = "";
  return spTokenRequest("grant_type=authorization_code&code=" + spUrlEncode(code) +
                        "&redirect_uri=" + spUrlEncode(SPOTIFY_REDIRECT));
}

bool spEnsureToken() {
  if (!spAccess.isEmpty() && millis() - spAccessAt + 60000 < spAccessTtl) return true;
  if (!spotifyReady()) return false;
  return spTokenRequest("grant_type=refresh_token&refresh_token=" + spUrlEncode(spRefresh));
}

// Запрос к Web API. path — после /v1. Возвращает HTTP-код, ответ — в resp
int spApi(const char *method, const String &path, const String &body, String &resp) {
  resp = "";
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!spEnsureToken()) return -1;
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);
    HTTPClient http;
    http.setReuse(false);  // ответ 204 без тела — с keep-alive getString() ждал бы до таймаута
    http.setConnectTimeout(8000);
    http.begin(client, "https://api.spotify.com/v1" + path);
    http.setTimeout(10000);
    http.addHeader("Authorization", "Bearer " + spAccess);
    if (!body.isEmpty()) http.addHeader("Content-Type", "application/json");
    uint32_t t0 = millis();
    Serial.printf("[spotify] %s %s…\n", method, path.substring(0, 60).c_str());
    int code = http.sendRequest(method, body);
    if (code > 0 && code != 204) resp = http.getString();
    http.end();
    Serial.printf("[spotify] -> %d за %lu мс, %u байт\n", code, (unsigned long)(millis() - t0), (unsigned)resp.length());
    if (code == 401 && attempt == 0) {  // токен протух раньше срока — обновляем и повторяем
      spAccess = "";
      continue;
    }
    return code;
  }
  return 401;
}

String spErrorText(int code, const String &resp) {
  if (code == -1) return String("Spotify не подключён: ") + spStatus;
  if (code == 403) return "Spotify не разрешил: нужен Premium, или это действие сейчас недоступно";
  if (code == 404) return "Не вижу активного устройства Spotify. Открой Spotify на телефоне или компьютере";
  if (code == 429) return "Spotify просит подождать — слишком много запросов";
  JsonDocument doc;
  deserializeJson(doc, resp);
  return String("Ошибка Spotify ") + code + ": " + (const char *)(doc["error"]["message"] | "нет связи");
}

// Устройство для воспроизведения: активное — как есть; если активного нет, берём сохранённое по имени
// или первое доступное. Пусто — устройств нет вовсе.
String spPickDevice(String &deviceName) {
  String resp;
  int code = spApi("GET", "/me/player/devices", "", resp);
  if (code != 200) return "";
  JsonDocument doc;
  deserializeJson(doc, resp);
  JsonArray list = doc["devices"].as<JsonArray>();
  String firstId, savedId;
  for (JsonObject d : list) {
    String id = d["id"] | "", name = d["name"] | "";
    if (id.isEmpty() || (d["is_restricted"] | false)) continue;
    if (d["is_active"] | false) {
      deviceName = name;
      return id;
    }
    if (firstId.isEmpty()) {
      firstId = id;
      deviceName = name;
    }
    if (!spDevice.isEmpty() && name.equalsIgnoreCase(spDevice)) savedId = id;
  }
  if (!savedId.isEmpty()) {
    deviceName = spDevice;
    return savedId;
  }
  return firstId;
}

// id устройства Spotify по имени (без учёта регистра); пусто — такого сейчас нет
String spFindDevice(const String &name) {
  String resp;
  if (name.isEmpty() || spApi("GET", "/me/player/devices", "", resp) != 200) return "";
  JsonDocument doc;
  deserializeJson(doc, resp);
  for (JsonObject d : doc["devices"].as<JsonArray>()) {
    String n = d["name"] | "";
    if (n.equalsIgnoreCase(name)) return d["id"] | "";
  }
  return "";
}

// Найти и включить. type: track, artist, album, playlist. "" — успех (что включили — в asActionInfo)
String spotifyPlay(const String &query, String type) {
  asActionInfo = "";
  if (query.isEmpty()) return "Скажи, что включить в Spotify";
  if (type != "artist" && type != "album" && type != "playlist") type = "track";

  String resp;
  int code = spApi("GET", "/search?q=" + spUrlEncode(query) + "&type=" + type + "&limit=5", "", resp);
  if (code != 200) return spErrorText(code, resp);
  String key = type + "s";  // tracks, artists, albums, playlists
  JsonDocument filter;
  filter[key]["items"][0]["uri"] = true;
  filter[key]["items"][0]["name"] = true;
  filter[key]["items"][0]["artists"][0]["name"] = true;
  filter[key]["items"][0]["owner"]["display_name"] = true;
  JsonDocument doc;
  deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  JsonObject item;
  for (JsonVariant v : doc[key]["items"].as<JsonArray>())  // в плейлистах бывают пустые элементы
    if (v.is<JsonObject>() && v["uri"].is<const char *>()) {
      item = v.as<JsonObject>();
      break;
    }
  if (item.isNull()) return "В Spotify ничего не нашлось по запросу «" + query + "»";

  String uri = item["uri"] | "", name = item["name"] | "";
  String who = item["artists"][0]["name"] | "";
  if (who.isEmpty()) who = item["owner"]["display_name"] | "";
  JsonDocument req;
  if (type == "track") req["uris"][0] = uri;
  else req["context_uri"] = uri;
  String body;
  serializeJson(req, body);

  String deviceName;
  if (!spStreamUrl.isEmpty()) {
    // Есть мост на ПК — играем всегда на нём, чтобы звук пришёл в колонку
    String dev = spFindDevice(spDevice);
    if (dev.isEmpty())
      return "Не вижу в Spotify устройство «" + spDevice + "». Запусти мост для колонки на компьютере";
    deviceName = spDevice;
    code = spApi("PUT", "/me/player/play?device_id=" + spUrlEncode(dev), body, resp);
  } else {
    code = spApi("PUT", "/me/player/play", body, resp);
  }
  if (code == 404 && spStreamUrl.isEmpty()) {  // нет активного устройства — выбираем сами и включаем на нём
    String dev = spPickDevice(deviceName);
    if (dev.isEmpty()) return "Не вижу ни одного устройства Spotify. Открой Spotify на телефоне или компьютере";
    code = spApi("PUT", "/me/player/play?device_id=" + spUrlEncode(dev), body, resp);
  }
  if (code != 200 && code != 202 && code != 204) return spErrorText(code, resp);

  asActionInfo = "Включено в Spotify: " + name + (who.isEmpty() ? "" : " — " + who) +
                 (deviceName.isEmpty() ? "" : " (на устройстве " + deviceName + ")");
  Serial.printf("[spotify] %s\n", asActionInfo.c_str());
  return "";
}

// pause, resume, next, previous
String spotifyControl(const String &action) {
  asActionInfo = "";
  String resp;
  int code;
  if (action == "pause") code = spApi("PUT", "/me/player/pause", "", resp);
  else if (action == "resume") code = spApi("PUT", "/me/player/play", "", resp);
  else if (action == "next") code = spApi("POST", "/me/player/next", "", resp);
  else if (action == "previous") code = spApi("POST", "/me/player/previous", "", resp);
  else return "Не понял команду для Spotify";
  if (code == 200 || code == 202 || code == 204) return "";
  return spErrorText(code, resp);
}

// Что сейчас играет — в asActionInfo
String spotifyNowPlaying() {
  asActionInfo = "";
  String resp;
  int code = spApi("GET", "/me/player/currently-playing", "", resp);
  if (code == 204) {
    asActionInfo = "В Spotify сейчас ничего не играет";
    return "";
  }
  if (code != 200) return spErrorText(code, resp);
  JsonDocument doc;
  deserializeJson(doc, resp);
  String name = doc["item"]["name"] | "", who = doc["item"]["artists"][0]["name"] | "";
  bool playing = doc["is_playing"] | false;
  asActionInfo = String(playing ? "Сейчас в Spotify играет: " : "В Spotify на паузе: ") + name +
                 (who.isEmpty() ? "" : " — " + who);
  return "";
}
