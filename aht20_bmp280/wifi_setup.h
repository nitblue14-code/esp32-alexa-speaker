// Wi-Fi и режим настройки. Сеть и пароль хранятся в памяти платы, а не в коде.
// Если после включения колонка 45 секунд не может подключиться (другой дом, новый роутер, неверный пароль),
// она сама создаёт открытую сеть «Alexa-Setup». Подключаешься к ней с телефона — открывается страница
// (или открой http://192.168.4.1), выбираешь свою сеть, вводишь пароль — колонка перезагружается и подключается.
// Ещё режим настройки включается, если держать кнопку BOOT 10 секунд.
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define SETUP_AP_NAME     "Alexa-Setup"
#define SETUP_AFTER_MS    45000                // столько ждём подключения после включения
#define SETUP_TIMEOUT_MS  (10 * 60 * 1000UL)   // 10 минут к сети настройки никто не подключён — перезагрузка,
                                               // пробуем свою сеть снова (роутер мог просто долго загружаться)

extern WebServer web;
void tgNotify(const String &text);  // telegram.h
extern Preferences prefs;

String wifiSsid, wifiPass;
bool wifiSetupActive = false;
bool wifiEverConnected = false;  // с момента включения хоть раз подключились
DNSServer setupDns;
uint32_t setupSeenAt = 0;        // когда к сети настройки последний раз кто-то был подключён

const char SETUP_HTML[] PROGMEM = R"rawliteral(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Alexa — Wi-Fi</title><style>
body{font-family:system-ui,sans-serif;background:#0b1220;color:#e5e7eb;margin:0;padding:16px}
.c{max-width:420px;margin:0 auto;background:#111a2e;border:1px solid #1f2a44;border-radius:16px;padding:18px}
h1{font-size:20px;margin:0 0 6px}p{color:#94a3b8;font-size:14px}
.n{display:flex;justify-content:space-between;gap:8px;padding:10px 12px;border:1px solid #1f2a44;border-radius:10px;margin:6px 0;cursor:pointer}
.n.on{border-color:#34d399}
input,button{width:100%;box-sizing:border-box;font-size:16px;padding:11px;border-radius:10px;border:1px solid #1f2a44;
background:#0b1220;color:#e5e7eb;margin-top:8px}
button{background:#34d399;color:#06281c;font-weight:700;border:0}
#st{color:#e5e7eb}
</style></head><body><div class="c">
<h1>🔊 Alexa — подключение к Wi-Fi</h1>
<p>Выбери свою сеть и введи пароль. Колонка перезагрузится и подключится к ней.</p>
<div id="list"><p>Ищу сети…</p></div>
<input id="ssid" placeholder="Название сети" autocomplete="off">
<input id="pass" type="password" placeholder="Пароль">
<button id="go">Подключить</button><p id="st"></p></div><script>
const $=id=>document.getElementById(id);
function scan(){fetch('/wifi/scan').then(r=>r.json()).then(l=>{
  if(l===null){setTimeout(scan,1500);return}
  const b=$('list');b.innerHTML='';
  if(!l.length)b.innerHTML='<p>Сетей не видно — впиши название вручную.</p>';
  for(const n of l){const d=document.createElement('div');d.className='n';
    const a=document.createElement('span'),s=document.createElement('span');
    a.textContent=n.s+(n.l?' 🔒':'');s.textContent=n.r>-60?'▂▄▆█':n.r>-75?'▂▄▆':'▂▄';d.append(a,s);
    d.onclick=()=>{document.querySelectorAll('.n').forEach(x=>x.classList.remove('on'));d.classList.add('on');
      $('ssid').value=n.s;$('pass').focus()};
    b.append(d)}
}).catch(()=>setTimeout(scan,2000))}
scan();
$('go').onclick=()=>{const s=$('ssid').value.trim();if(!s){$('st').textContent='Выбери сеть';return}
  $('go').disabled=true;
  fetch('/wifi/save',{method:'POST',body:new URLSearchParams({ssid:s,pass:$('pass').value})})
    .then(r=>r.text()).then(t=>$('st').textContent=t)
    .catch(()=>$('st').textContent='Сохранено, колонка перезагружается…')};
</script></body></html>)rawliteral";

void wifiLoad() {
  // Первый запуск прошивки с этой функцией — переносим сеть, которая раньше была прописана в коде
  if (!prefs.getBool("wmig", false)) {
    prefs.putString("wssid", WIFI_SSID);
    prefs.putString("wpass", WIFI_PASSWORD);
    prefs.putBool("wmig", true);
  }
  wifiSsid = prefs.getString("wssid", "");
  wifiPass = prefs.getString("wpass", "");
}

// Список сетей вокруг. Поиск идёт в фоне: пока не закончился — отдаём null, страница спросит ещё раз.
void handleWifiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);
  if (n < 0) {
    web.send(200, "application/json", "null");
    return;
  }
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    bool dup = s.isEmpty();
    for (JsonObject o : list) dup = dup || o["s"] == s;
    if (dup) continue;  // скрытые сети и повторы (один роутер на нескольких точках)
    JsonObject o = list.add<JsonObject>();
    o["s"] = s;
    o["r"] = WiFi.RSSI(i);
    o["l"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String json;
  serializeJson(doc, json);
  web.send(200, "application/json", json);
}

// Сохранить сеть и перезагрузиться. Работает и со страницы настройки, и из «Настроек» на обычном сайте.
void handleWifiSave() {
  String ssid = web.arg("ssid");
  ssid.trim();
  if (ssid.isEmpty()) {
    web.send(400, "text/plain; charset=utf-8", "Не указана сеть");
    return;
  }
  prefs.putString("wssid", ssid);
  prefs.putString("wpass", web.arg("pass"));
  prefs.putBool("wnew", true);  // после подключения скажем, что всё получилось
  Serial.printf("Wi-Fi: новая сеть «%s» — перезагружаюсь\n", ssid.c_str());
  web.send(200, "text/plain; charset=utf-8",
           "Сохранил. Колонка перезагрузится и подключится к «" + ssid + "» — скажет об этом голосом. "
           "Подключи телефон обратно к своей сети и открой http://" HOST_NAME ".local. "
           "Если пароль неверный — через минуту снова появится сеть " SETUP_AP_NAME ".");
  delay(1500);
  ESP.restart();
}

void wifiSetupStart() {
  if (wifiSetupActive) return;
  wifiSetupActive = true;
  setupSeenAt = millis();
  Serial.printf("Режим настройки Wi-Fi: подключись к сети «%s» и открой http://192.168.4.1\n", SETUP_AP_NAME);
  WiFi.disconnect();  // свою сеть пока не ищем: попытки подключения мешают точке доступа
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_NAME);
  setupDns.start(53, "*", WiFi.softAPIP());  // любой адрес ведёт на колонку — телефон сам откроет страницу
  WiFi.scanNetworks(true);
  web.on("/", [] { web.send_P(200, "text/html; charset=utf-8", SETUP_HTML); });
  web.on("/wifi/scan", handleWifiScan);
  web.on("/wifi/save", HTTP_POST, handleWifiSave);
  web.onNotFound([] {
    web.sendHeader("Location", "http://192.168.4.1/");
    web.send(302, "text/plain", "");
  });
  web.begin();
}

// Держали BOOT 10 секунд — перезагрузиться сразу в режим настройки
void wifiRequestSetup() {
  Serial.println("Кнопка BOOT 10 с — перезагружаюсь в режим настройки Wi-Fi");
  prefs.putBool("wsetup", true);
  delay(300);
  ESP.restart();
}

void setupWiFi() {
  wifiLoad();
  WiFi.setHostname(HOST_NAME);
  WiFi.setSleep(false);  // без энергосбережения: радио и голос не заикаются, «Alexa» отвечает быстрее
  bool forced = prefs.getBool("wsetup", false);
  if (forced) prefs.remove("wsetup");
  if (forced || wifiSsid.isEmpty()) {
    wifiSetupStart();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.printf("Подключаюсь к Wi-Fi %s…\n", wifiSsid.c_str());
}

// Из loop(): переподключение, а если с включения так и не подключились — режим настройки
void checkWiFi() {
  if (wifiSetupActive) {
    setupDns.processNextRequest();
    web.handleClient();
    if (WiFi.softAPgetStationNum() > 0) setupSeenAt = millis();
    if (millis() - setupSeenAt > SETUP_TIMEOUT_MS) {
      Serial.println("Режим настройки: никто не подключился — перезагружаюсь и пробую свою сеть снова");
      ESP.restart();
    }
    return;
  }

  static uint32_t lostSince = 0, lostAt = 0;
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected && wasConnected) lostAt = millis();
  if (connected && !wasConnected) {
    Serial.printf("Wi-Fi подключён, IP %s\n", WiFi.localIP().toString().c_str());
    if (wifiEverConnected && millis() - lostAt > 120000)  // пропадал больше 2 минут — расскажем в Telegram
      tgNotify("📶 Wi-Fi пропадал на " + String((millis() - lostAt) / 60000) + " мин, теперь снова на связи.");
    wifiEverConnected = true;
    if (prefs.getBool("wnew", false)) {  // только что настроили сеть — скажем, что получилось
      prefs.remove("wnew");
      sendAudioCmd(AUDIO_SAY, "Подключилась к сети " + wifiSsid + ". Моя страница: " HOST_NAME " точка локал.");
    }
  }
  wasConnected = connected;
  if (connected) {
    lostSince = 0;
    return;
  }
  if (!wifiEverConnected && millis() > SETUP_AFTER_MS) {
    wifiSetupStart();
    return;
  }
  if (!lostSince) lostSince = millis();
  if (millis() - lostSince > 30000) {  // Wi-Fi пропал надолго (роутер перезагрузился) — переподключаемся сами
    lostSince = millis();
    Serial.println("Wi-Fi нет уже 30 с — переподключаюсь");
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
}
