// HTML-страницы вынесены сюда: препроцессор Arduino ломает raw-строки с JavaScript внутри .ino
#pragma once
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Alexa</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<style>
:root{
  --bg:#07120d;--bg2:#0c1d15;--card:#102419;--card2:#143023;--line:#1f4433;
  --txt:#e6f4ec;--mut:#8fb5a1;--acc:#34d399;--acc2:#10b981;--accTxt:#03160d;
  --warn:#fbbf24;--bad:#f87171;--temp:#fb923c;--hum:#38bdf8;--press:#a3e635;
  --shadow:0 8px 24px rgba(0,0,0,.35);
}
@media (prefers-color-scheme:light){:root{
  --bg:#eef8f1;--bg2:#e2f3e8;--card:#ffffff;--card2:#f3faf5;--line:#cfe7d7;
  --txt:#0d2a1b;--mut:#4f7562;--acc:#16a34a;--acc2:#15803d;--accTxt:#ffffff;
  --shadow:0 6px 18px rgba(16,80,40,.08);
}}
*{box-sizing:border-box}
[hidden]{display:none!important}
html,body{margin:0}
body{background:radial-gradient(1200px 600px at 10% -10%,var(--bg2),transparent),var(--bg);color:var(--txt);
  font-family:-apple-system,"Segoe UI",system-ui,sans-serif;min-height:100vh}
.wrap{max-width:1280px;margin:0 auto;padding:16px}

/* Шапка */
header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:16px}
.brand{display:flex;align-items:center;gap:10px;font-size:22px;font-weight:700;letter-spacing:.2px}
.brand .dot{width:34px;height:34px;border-radius:10px;background:linear-gradient(135deg,var(--acc),var(--acc2));
  display:grid;place-items:center;color:var(--accTxt);font-size:18px;box-shadow:var(--shadow)}
.chips{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.chip{background:var(--card);border:1px solid var(--line);border-radius:999px;padding:6px 12px;font-size:13px;color:var(--mut)}
.chip a{color:var(--acc);text-decoration:none}
.chip label{cursor:pointer}
.gear{cursor:pointer;font:inherit;font-size:13px}
.gear.on{background:var(--acc);color:var(--accTxt);border-color:var(--acc)}
.airw{border-color:var(--warn);color:var(--warn);font-weight:600;padding:12px 16px}

/* Сетка блоков */
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
.card{background:linear-gradient(180deg,var(--card),var(--card2));border:1px solid var(--line);border-radius:18px;
  padding:16px;box-shadow:var(--shadow);min-width:0}
.c3{grid-column:span 3}.c4{grid-column:span 4}.c5{grid-column:span 5}.c6{grid-column:span 6}.c7{grid-column:span 7}.c12{grid-column:span 12}
@media (max-width:1000px){.c3{grid-column:span 6}.c4,.c5,.c6,.c7{grid-column:span 12}}
@media (max-width:560px){.c3{grid-column:span 12}}
.title{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:12px;flex-wrap:wrap}
.title h2{margin:0;font-size:15px;font-weight:650;letter-spacing:.3px}
.title .tag{font-size:12px;color:var(--mut)}

/* Плитки погоды */
.tile .lbl{color:var(--mut);font-size:13px;display:flex;align-items:center;gap:6px}
.tile .big{font-size:34px;font-weight:700;margin-top:6px;line-height:1.1}
.tile .sub{margin-top:4px}
.tile.temp .big{color:var(--temp)}.tile.hum .big{color:var(--hum)}.tile.press .big{color:var(--press)}
.tile.fcst .big{font-size:17px;font-weight:600;line-height:1.35}

.lbl{color:var(--mut);font-size:13px}
.sub{color:var(--mut);font-size:13px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:10px}
.row>.lbl:first-child{min-width:110px}
@media (max-width:560px){.row>.lbl:first-child{min-width:0;flex-basis:100%}}
input[type=text],select{flex:1;min-width:0;background:var(--bg);color:var(--txt);border:1px solid var(--line);
  border-radius:12px;padding:10px 12px;font-size:15px;outline:none}
input[type=text]:focus,select:focus{border-color:var(--acc)}
input[type=range]{flex:1;min-width:80px;accent-color:var(--acc)}
input[type=checkbox]{accent-color:var(--acc);width:16px;height:16px;vertical-align:-3px}
.btn{background:linear-gradient(135deg,var(--acc),var(--acc2));color:var(--accTxt);border:0;border-radius:12px;
  padding:10px 16px;font-size:15px;font-weight:600;cursor:pointer;transition:transform .08s,filter .15s}
.btn:hover{filter:brightness(1.08)}.btn:active{transform:scale(.97)}
.btn:disabled{opacity:.5;cursor:default}
.btn.ghost{background:transparent;color:var(--txt);border:1px solid var(--line);font-weight:500}
.btn.ghost:hover{border-color:var(--acc)}
.val{font-variant-numeric:tabular-nums;min-width:52px;text-align:right;color:var(--txt);font-size:13px}
details{margin-top:12px;border-top:1px solid var(--line);padding-top:10px}
summary{cursor:pointer;color:var(--mut);font-size:13px}

/* Ассистент */
.mic{width:64px;height:64px;border-radius:50%;font-size:26px;padding:0;flex:none;
  box-shadow:0 0 0 6px color-mix(in srgb,var(--acc) 18%,transparent)}
.mic.on{animation:pulse 1.2s infinite}
@keyframes pulse{50%{box-shadow:0 0 0 14px color-mix(in srgb,var(--acc) 6%,transparent)}}
.state{font-size:15px;font-weight:600}
.bubble{background:var(--bg);border:1px solid var(--line);border-radius:14px;padding:10px 12px;margin-top:10px}
.bubble .lbl{font-size:12px;margin-bottom:4px}
.bubble.me{border-left:3px solid var(--hum)}.bubble.ai{border-left:3px solid var(--acc)}
.err{color:var(--bad);font-size:13px;margin-top:6px}
.ev{display:flex;align-items:center;gap:8px;background:var(--bg);border:1px solid var(--line);border-radius:12px;padding:6px 10px;margin-top:6px}
.ev span{flex:1;font-size:14px}
.ev .btn{padding:4px 10px}

/* Музыка */
.now{background:var(--bg);border:1px solid var(--line);border-radius:14px;padding:12px;display:flex;gap:12px;align-items:center}
.now .disc{width:42px;height:42px;border-radius:50%;flex:none;
  background:conic-gradient(var(--acc),var(--acc2),var(--hum),var(--acc));opacity:.85}
.now.playing .disc{animation:spin 3s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.now .info{min-width:0}.now .info div{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#found .btn{display:block;width:100%;text-align:left;margin-top:6px}

/* Микрофон */
.meter{flex:1;height:10px;background:var(--bg);border:1px solid var(--line);border-radius:6px;overflow:hidden}
.meter div{height:100%;width:0;background:linear-gradient(90deg,var(--acc2),var(--acc));transition:width .15s}

/* Графики */
.tabs{display:flex;gap:6px;flex-wrap:wrap}
.tabs button{background:var(--bg);color:var(--mut);border:1px solid var(--line);border-radius:999px;padding:6px 12px;font-size:13px;cursor:pointer}
.tabs button.on{background:var(--acc);color:var(--accTxt);border-color:var(--acc)}
.charts{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
@media (max-width:1000px){.charts{grid-template-columns:1fr}}
.chart{height:220px;background:var(--bg);border:1px solid var(--line);border-radius:14px;padding:8px}
footer{color:var(--mut);font-size:12px;text-align:center;margin:18px 0 6px}
</style></head><body><div class="wrap">

<header>
  <div class="brand"><div class="dot">🔊</div>Alexa</div>
  <div class="chips">
    <span class="chip" id="info">подключаюсь…</span>
    <span class="chip"><label><input type="checkbox" id="led"> Светодиод</label></span>
    <button class="chip gear" id="night" title="Ночной режим: светодиод гаснет, голос тише, без предупреждений; до 7 утра">🌙 Ночь</button>
    <span class="chip"><a href="/update">⬆ Прошивка</a></span>
    <button class="chip gear" id="gear" title="Настройки">⚙ Настройки</button>
  </div>
</header>

<!-- Главная: погода, ассистент, музыка, история -->
<div class="grid" id="mainv">
  <!-- Погода -->
  <div class="card tile temp c3"><div class="lbl">🌡 Температура</div><div class="big" id="t">—</div><div class="sub">в комнате</div></div>
  <div class="card tile hum c3"><div class="lbl">💧 Влажность</div><div class="big" id="h">—</div><div class="sub">комфорт 40–60 %</div></div>
  <div class="card tile press c3"><div class="lbl">🧭 Давление</div><div class="big" id="p">—</div><div class="sub" id="ph"></div></div>
  <div class="card tile fcst c3"><div class="lbl">⛅ Прогноз по давлению</div><div class="big" id="fc">—</div><div class="sub" id="dp"></div></div>
  <div class="card c12 airw" id="airw" hidden>⚠ <span id="airt"></span></div>

  <!-- Ассистент -->
  <div class="card c7">
    <div class="title"><h2>🤖 Ассистент</h2><span class="tag" id="wakest"></span></div>
    <div class="row" style="margin-top:0">
      <button class="btn mic" id="talk" title="Говорить">🎤</button>
      <div style="flex:1;min-width:0">
        <div class="state" id="asst">или скажи «Alexa», или нажми BOOT</div>
        <div class="sub">Можно перебить кнопкой или словом «Alexa»</div>
      </div>
      <button class="btn ghost" id="asstop">■ Стоп</button>
    </div>
    <div class="bubble me"><div class="lbl">Вы</div><div id="heard">—</div></div>
    <div class="bubble ai"><div class="lbl">Ассистент</div><div id="answer">—</div></div>
    <div class="err" id="aserr"></div>
    <div class="row"><input type="text" id="ask" placeholder="Или напиши вопрос текстом…"><button class="btn" id="askb">Спросить</button></div>
    <div class="lbl" style="margin-top:14px">⏰ Таймеры, будильники, напоминания</div>
    <div id="events"><div class="sub">—</div></div>
  </div>

  <!-- Музыка -->
  <div class="card c5">
    <div class="title"><h2>🎵 Музыка</h2><span class="tag" id="playing"></span></div>
    <div class="now" id="nowbox"><div class="disc"></div>
      <div class="info"><div id="nowplay" style="font-weight:600">Ничего не играет</div><div class="sub" id="nowtitle"></div></div></div>
    <div class="row">
      <select id="st"><option value="custom">Своя ссылка…</option></select>
      <button class="btn" id="play">▶</button><button class="btn ghost" id="stop">■</button>
    </div>
    <div class="row" id="urlrow" hidden><input type="text" id="url" placeholder="http://… ссылка на поток mp3/aac"></div>
    <div class="row"><span class="lbl">🔊 Громкость</span><input type="range" id="vol" min="0" max="21"><span class="val" id="volv"></span></div>
    <div class="row"><input type="text" id="rq" placeholder="Найти: jazz, rock, Radio Record…"><button class="btn ghost" id="rqb">Найти</button></div>
    <div id="found"></div>
  </div>

  <!-- Графики -->
  <div class="card c12">
    <div class="title"><h2>📈 История</h2>
      <div class="tabs" id="tabs">
        <button data-h="6">6 часов</button><button data-h="24" class="on">Сутки</button>
        <button data-h="72">3 дня</button><button data-h="168">Неделя</button><button data-h="336">2 недели</button>
      </div></div>
    <div class="charts" id="charts">
      <div class="chart"><canvas id="ct"></canvas></div>
      <div class="chart"><canvas id="ch"></canvas></div>
      <div class="chart"><canvas id="cp"></canvas></div>
    </div>
  </div>

</div>

<!-- Вкладка «Настройки» (шестерёнка в шапке) -->
<div class="grid" id="setv" hidden>
  <!-- Голос -->
  <div class="card c4">
    <div class="title"><h2>🗣 Голос ассистента</h2></div>
    <div class="row" style="margin-top:0"><span class="lbl">Голос</span>
      <select id="voice">
        <option value="ru-RU-Chirp3-HD-Leda">Leda</option>
        <option value="ru-RU-Chirp3-HD-Kore">Kore</option>
        <option value="ru-RU-Chirp3-HD-Aoede">Aoede</option>
        <option value="ru-RU-Chirp3-HD-Sulafat">Sulafat</option>
        <option value="ru-RU-Chirp3-HD-Achird">Achird</option>
        <option value="ru-RU-Chirp3-HD-Charon">Charon (мужской)</option>
        <option value="ru-RU-Chirp3-HD-Puck">Puck</option>
        <option value="">Google Translate (простой)</option>
      </select></div>
    <div class="row"><span class="lbl">🔊 Громкость голоса</span><input type="range" id="vvol" min="0" max="21"><span class="val" id="vvolv"></span></div>
    <div class="row"><span class="lbl">⚡ Усиление</span><input type="range" id="vboost" min="-6" max="12" step="1"><span class="val" id="vboostv"></span></div>
    <div class="sub">Громкость голоса своя — не зависит от музыки. Усиление делает голос громче без хрипа.</div>
    <div class="sub" id="ttsst" style="margin-top:6px"></div>
    <div class="row"><input type="text" id="say" maxlength="200" placeholder="Что сказать…"><button class="btn" id="sayb">Сказать</button></div>
    <div class="row"><button class="btn ghost" id="wx" style="flex:1">☀ Озвучить погоду</button></div>
    <div class="row"><label class="lbl"><input type="checkbox" id="airv"> Предупреждать голосом о воздухе в комнате</label></div>
    <div class="sub">Сухо, сыро, холодно, жарко или резко падает давление — скажет, если держится 10 минут. Только с 9 до 21 и не в ночном режиме, о том же — не чаще раза в 3 часа.</div>
  </div>

  <!-- Alexa -->
  <div class="card c4">
    <div class="title"><h2>👂 Слово «Alexa»</h2></div>
    <div class="row" style="margin-top:0"><label class="lbl"><input type="checkbox" id="wake"> Слушать «Alexa»</label></div>
    <div class="row"><label class="lbl"><input type="checkbox" id="aec"> Эхоподавление — слышит сквозь музыку</label></div>
    <div class="row"><span class="lbl">Чувствительность</span><input type="range" id="thr" min="0.4" max="0.9" step="0.05"><span class="val" id="thrv"></span></div>
    <div class="sub">Левее — откликается охотнее, но может срабатывать сама. Правее — строже.</div>
    <div class="row"><button class="btn ghost" id="recafe" style="flex:1">● Записать 5 сек того, что слышит «Alexa»</button></div>
  </div>

  <!-- Микрофон -->
  <div class="card c4">
    <div class="title"><h2>🎙 Микрофон</h2><span class="tag" id="recst"></span></div>
    <div class="row" style="margin-top:0"><span class="lbl">Уровень</span><div class="meter"><div id="lvl"></div></div><span class="val" id="lvlv">0</span></div>
    <div class="row"><span class="lbl">Усиление</span><input type="range" id="mgain" min="1" max="32" step="0.5"><span class="val" id="mgainv"></span></div>
    <div class="row"><span class="lbl">Шумоподавление</span><input type="range" id="mnoise" min="0" max="100"><span class="val" id="mnoisev"></span></div>
    <div class="row"><button class="btn ghost" id="rec" style="flex:1">● Записать 5 сек</button></div>
    <div class="row"><audio id="player" controls hidden style="width:100%"></audio></div>
  </div>

  <!-- Wi-Fi -->
  <div class="card c12">
    <div class="title"><h2>📶 Wi-Fi</h2><span class="tag" id="wifist"></span></div>
    <div class="row" style="margin-top:0"><input type="text" id="wssid" placeholder="Название новой сети" autocomplete="off">
      <input type="password" id="wpass" placeholder="Пароль"><button class="btn ghost" id="wsave">Подключить</button></div>
    <div class="sub">Колонка перезагрузится и подключится к новой сети. Если не получится — сама включит сеть
      «Alexa-Setup»: подключись к ней с телефона и выбери сеть там. Режим настройки включается и так:
      держи кнопку BOOT 10 секунд.</div>
    <div class="row"><a class="btn ghost" href="/factory" style="flex:1;text-align:center;text-decoration:none">⚠ Сброс к заводским (перед подарком)</a></div>
  </div>

  <!-- Telegram -->
  <div class="card c12">
    <div class="title"><h2>✈ Telegram — управление издалека</h2><span class="tag" id="tgst"></span></div>
    <div class="row" style="margin-top:0"><input type="text" id="tgtoken" placeholder="Токен бота от @BotFather (123456:ABC…)" autocomplete="off">
      <button class="btn ghost" id="tgsave">Сохранить</button></div>
    <div class="sub">Создай бота: в Telegram открой @BotFather → /newbot → придумай имя → вставь сюда токен.
      Потом напиши своему боту код привязки: <b id="tgcode">—</b> (код меняется после каждой привязки).</div>
    <div id="tgchats"></div>
  </div>

  <!-- Настройки -->
  <div class="card c12">
    <div class="title"><h2>⚙ Настройки</h2></div>
    <div class="grid">
      <div class="c6">
        <div class="lbl">Groq — распознавание речи и ответы</div>
        <div class="row"><input type="text" id="gkey" placeholder="API-ключ Groq (gsk_…)" autocomplete="off"><button class="btn ghost" id="gsave">Сохранить</button></div>
        <div class="sub" id="gkeyst"></div>
        <div class="row"><input type="text" id="gmodel" placeholder="Модель"><button class="btn ghost" id="gmsave">Сохранить</button></div>
        <div class="sub">Модель меняй, только если Groq пишет, что текущая больше не поддерживается. Пусто — по умолчанию.</div>
      </div>
      <div class="c6">
        <div class="lbl">Google Cloud — красивый голос</div>
        <div class="row"><input type="text" id="gckey" placeholder="Ключ API Google Cloud (AIza…)" autocomplete="off"><button class="btn ghost" id="gcsave">Сохранить</button></div>
        <div class="sub" id="gckeyst"></div>
        <div class="sub" id="gcuse"></div>
      </div>
      <div class="c12" style="border-top:1px solid var(--line);padding-top:12px">
        <div class="lbl">⚡ Gemini Live — живой разговор голосом (быстрее и естественнее; ключ из AI Studio, бесплатный проект)</div>
        <div class="row"><label class="lbl"><input type="checkbox" id="liveon"> Включён</label>
          <span class="lbl">Голос</span>
          <select id="livevoice" style="max-width:200px">
            <option>Kore</option><option>Leda</option><option>Aoede</option><option>Sulafat</option>
            <option>Achird</option><option>Charon</option><option>Puck</option><option>Zephyr</option>
          </select></div>
        <div class="row"><input type="text" id="livekey" placeholder="Ключ Gemini API (AIza…) — из проекта без оплаты" autocomplete="off"><button class="btn ghost" id="livesave">Сохранить</button></div>
        <div class="row"><input type="text" id="livemodel" placeholder="Модель"><button class="btn ghost" id="livemsave">Сохранить</button></div>
        <div class="sub" id="livest"></div>
        <div class="sub">Если Gemini недоступен, колонка сама отвечает по-старому (Groq + голос Google Cloud).</div>
      </div>
      <div class="c12" style="border-top:1px solid var(--line);padding-top:12px">
        <div class="lbl">🎵 Spotify — песни голосом («Алекса, включи Океан Ельзи»), нужен Premium</div>
        <div class="sub">1. На <a href="https://developer.spotify.com/dashboard" target="_blank">developer.spotify.com</a>
          создай приложение (Web API), в Redirect URI впиши <b id="spredir">http://127.0.0.1:8888/callback</b>.</div>
        <div class="row"><input type="text" id="spid" placeholder="Client ID" autocomplete="off">
          <input type="text" id="spsec" placeholder="Client Secret" autocomplete="off">
          <button class="btn ghost" id="spsave">Сохранить</button></div>
        <div class="sub">2. <a href="#" id="splogin" target="_blank">Войти в Spotify</a> и разрешить доступ. Откроется
          страница с ошибкой «не удаётся открыть 127.0.0.1» — так и должно быть: скопируй её адрес целиком и вставь сюда.</div>
        <div class="row"><input type="text" id="spcode" placeholder="http://127.0.0.1:8888/callback?code=…" autocomplete="off">
          <button class="btn ghost" id="spcodesave">Подключить</button></div>
        <div class="row"><input type="text" id="spdev" placeholder="Устройство по умолчанию (необязательно), например iPhone">
          <button class="btn ghost" id="spdevsave">Сохранить</button><button class="btn ghost" id="splogout">Выйти</button></div>
        <div class="sub">3. Чтобы музыка шла из колонки — запусти на ПК мост (папка pc_bridge) и впиши адрес его потока.
          Пусто — музыка играет там, где открыт Spotify (телефон, ПК).</div>
        <div class="row"><input type="text" id="spstream" placeholder="http://192.168.31.X:8000/spotify.mp3" autocomplete="off">
          <button class="btn ghost" id="spstreamsave">Сохранить</button></div>
        <div class="sub" id="spst"></div>
      </div>
    </div>
  </div>
</div>
<footer>Alexa · ESP32-S3 · http://meteo.local · <a href="/log" style="color:var(--acc)">журнал</a></footer>
</div>

<script>
const $=id=>document.getElementById(id);
// Вкладки: главная и «Настройки» (шестерёнка). #settings в адресе — чтобы после обновления остаться там
function showTab(set){$('mainv').hidden=set;$('setv').hidden=!set;$('gear').classList.toggle('on',set);
  $('gear').textContent=set?'← Главная':'⚙ Настройки';history.replaceState(null,'',set?'#settings':location.pathname)}
$('gear').onclick=()=>showTab($('setv').hidden);
showTab(location.hash==='#settings');
const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v).trim();
let H=24;
const p2=n=>String(n).padStart(2,'0');
const dbText=v=>((+v)>0?'+':'')+(+v).toFixed(0)+' дБ';
function fmt(ms,full){const d=new Date(ms),hm=p2(d.getHours())+':'+p2(d.getMinutes());
  return (H>24||full)?p2(d.getDate())+'.'+p2(d.getMonth()+1)+' '+hm:hm}
const hasChart=typeof Chart!=='undefined';
function mk(id,label,color){
  const mut=css('--mut'),grid=css('--line');
  return new Chart($(id),{type:'line',
    data:{datasets:[{label,data:[],borderColor:color,backgroundColor:color+'26',fill:true,pointRadius:0,borderWidth:2,tension:.35}]},
    options:{responsive:true,maintainAspectRatio:false,animation:false,interaction:{mode:'index',intersect:false},
      plugins:{legend:{labels:{color:mut,boxWidth:10}},tooltip:{callbacks:{title:i=>fmt(i[0].parsed.x,true)}}},
      scales:{x:{type:'linear',ticks:{color:mut,maxTicksLimit:5,callback:v=>fmt(v)},grid:{color:grid}},
              y:{ticks:{color:mut},grid:{color:grid}}}}});
}
let cT,cH,cP;
if(hasChart){cT=mk('ct','Температура, °C',css('--temp'));cH=mk('ch','Влажность, %',css('--hum'));cP=mk('cp','Давление, мм рт. ст.',css('--press'))}
else{$('charts').innerHTML='<div class="sub">Графики не загрузились — нужен интернет на телефоне/компьютере.</div>'}

async function loadNow(){
  try{
    const r=await(await fetch('/api/now')).json();
    $('t').textContent=r.t.toFixed(1)+' °C';
    $('h').textContent=r.h.toFixed(0)+' %';
    $('p').textContent=(r.p*0.750062).toFixed(0)+' мм';
    $('ph').textContent=r.p.toFixed(1)+' гПа';
    $('fc').textContent=r.fc;
    $('dp').textContent=r.dp===null?'':'За 3 часа: '+(r.dp>0?'+':'')+r.dp.toFixed(1)+' гПа';
    $('led').checked=!!r.led;
    $('wifist').textContent=r.ssid?'сейчас: '+r.ssid+' · '+r.rssi+' dBm':'';
    $('night').classList.toggle('on',!!r.night);
    $('night').textContent=r.night?'🌙 Ночь до '+r.night:'🌙 Ночь';
    $('airw').hidden=!r.air;$('airt').textContent=r.air?'В комнате: '+r.air:'';
    $('airv').checked=!!r.airv;
    if(!volInit){$('vol').value=r.vol;$('volv').textContent=r.vol;$('voice').value=r.voice;
      $('vvol').value=r.vvol;$('vvolv').textContent=r.vvol;
      $('vboost').value=r.vboost;$('vboostv').textContent=dbText(r.vboost);
      volInit=true}
    $('ttsst').textContent=r.ttsStatus||'';
    $('gckeyst').textContent=r.gcKey?'Ключ сохранён: '+r.gcKey:'Ключ не задан — сейчас простой голос';
    $('gcuse').textContent='В этом месяце: '+r.gcChars.toLocaleString('ru')+' из '+r.gcLimit.toLocaleString('ru')+
      ' бесплатных символов (потом до 1-го числа — простой голос)';
    $('playing').textContent=(r.play?'● играет':'')+(r.sleep?' · 💤 выключу в '+r.sleep:'');
    $('nowplay').textContent=r.station||'Ничего не играет';
    $('nowtitle').textContent=r.station?(r.title||''):'';
    $('nowbox').classList.toggle('playing',!!(r.station&&r.play));
    last=r;
    const u=r.up;
    $('info').textContent='📶 '+r.rssi+' dBm · ⏱ '+(u>=86400?Math.floor(u/86400)+' д ':'')+Math.floor(u%86400/3600)+' ч '+Math.floor(u%3600/60)+' мин';
  }catch(e){$('info').textContent='⚠ Нет связи с метеостанцией'}
}
async function loadHist(){
  if(!hasChart)return;
  try{
    const r=await(await fetch('/api/history?h='+H)).json(),now=Date.now(),T=[],Hu=[],P=[];
    for(const [s,t,h,p] of r.d){const x=now-(r.up-s)*1000;T.push({x,y:t/100});Hu.push({x,y:h/10});P.push({x,y:+(p/10*0.750062).toFixed(1)})}
    cT.data.datasets[0].data=T;cH.data.datasets[0].data=Hu;cP.data.datasets[0].data=P;
    for(const c of [cT,cH,cP]){c.options.scales.x.min=now-H*3600000;c.options.scales.x.max=now;c.update()}
  }catch(e){}
}
$('tabs').onclick=e=>{const b=e.target.closest('button');if(!b)return;
  document.querySelectorAll('#tabs button').forEach(x=>x.classList.toggle('on',x===b));H=+b.dataset.h;loadHist()};
$('wsave').onclick=()=>{const s=$('wssid').value.trim();if(!s)return;
  if(!confirm('Переключить колонку на сеть «'+s+'»? Она перезагрузится.'))return;
  fetch('/wifi/save',{method:'POST',body:new URLSearchParams({ssid:s,pass:$('wpass').value})})
    .then(r=>r.text()).then(alert).catch(()=>alert('Колонка перезагружается…'))};
$('led').onchange=e=>fetch('/api/led?on='+(e.target.checked?1:0));
$('night').onclick=()=>fetch('/api/night?on='+(last&&last.night?0:1)).then(loadNow);
$('airv').onchange=e=>fetch('/api/air?voice='+(e.target.checked?1:0));

// Музыка и голос
let volInit=false,last=null;
const audio=q=>fetch('/api/audio?'+q).then(()=>setTimeout(loadNow,1500));
const say=t=>audio('a=say&text='+encodeURIComponent(t));
$('st').onchange=()=>{$('urlrow').hidden=$('st').value!=='custom'};
const playUrl=(u,name)=>audio('a=play&url='+encodeURIComponent(u)+'&name='+encodeURIComponent(name||''));
$('play').onclick=()=>{const custom=$('st').value==='custom',u=custom?$('url').value.trim():$('st').value;
  if(u)playUrl(u,custom?'':$('st').selectedOptions[0].textContent)};
$('stop').onclick=()=>audio('a=stop');
$('rqb').onclick=async()=>{const q=$('rq').value.trim();if(!q)return;
  $('found').innerHTML='<div class="sub" style="margin-top:8px">ищу…</div>';
  try{
    const list=await(await fetch('/api/radio?q='+encodeURIComponent(q))).json();
    $('found').innerHTML='';
    if(!list.length){$('found').innerHTML='<div class="sub" style="margin-top:8px">ничего не нашлось</div>';return}
    for(const s of list){const b=document.createElement('button');b.className='btn ghost';
      b.textContent='▶ '+s.name;b.onclick=()=>playUrl(s.url,s.name);$('found').append(b)}
  }catch(e){$('found').innerHTML='<div class="sub" style="margin-top:8px">каталог не ответил</div>'}};
$('rq').onkeydown=e=>{if(e.key==='Enter')$('rqb').click()};
$('vol').oninput=e=>$('volv').textContent=e.target.value;
$('vol').onchange=e=>fetch('/api/audio?a=vol&v='+e.target.value);
$('vvol').oninput=e=>$('vvolv').textContent=e.target.value;
$('vvol').onchange=e=>fetch('/api/audio?a=vvol&v='+e.target.value).then(()=>say('Так меня слышно?'));
$('vboost').oninput=e=>$('vboostv').textContent=dbText(e.target.value);
$('vboost').onchange=e=>fetch('/api/audio?a=vboost&v='+e.target.value).then(()=>say('Так меня слышно?'));
$('voice').onchange=e=>fetch('/api/audio?a=voice&v='+encodeURIComponent(e.target.value))
  .then(()=>say('Привет! Теперь я буду говорить этим голосом.'));
$('gcsave').onclick=()=>{const k=$('gckey').value.trim();if(!k)return;
  fetch('/api/gcloud',{method:'POST',body:new URLSearchParams({key:k})}).then(()=>{$('gckey').value='';
    say('Проверка голоса. Если голос стал живым и приятным, всё настроено правильно.')})};
$('sayb').onclick=()=>{const t=$('say').value.trim();if(t)say(t)};
$('say').onkeydown=e=>{if(e.key==='Enter')$('sayb').click()};
// 1 градус, 2 градуса, 5 градусов
const plural=(n,one,few,many)=>{const a=Math.abs(n)%100,b=a%10;
  return n+' '+(a>10&&a<20?many:b===1?one:b>=2&&b<=4?few:many)};
$('wx').onclick=()=>{if(!last)return;
  say('Температура '+plural(Math.round(last.t),'градус','градуса','градусов')+
      '. Влажность '+plural(Math.round(last.h),'процент','процента','процентов')+
      '. Давление '+plural(Math.round(last.p*0.750062),'миллиметр','миллиметра','миллиметров')+' ртутного столба. '+last.fc)};
// Станции — из общего списка на плате, «Своя ссылка» остаётся последней
fetch('/api/stations').then(r=>r.json()).then(list=>{
  for(const s of list.reverse()){const o=document.createElement('option');o.value=s.url;o.textContent=s.name;$('st').prepend(o)}
  $('st').selectedIndex=0}).catch(()=>{});

// Микрофон: уровень обновляется постоянно, во время записи показываем секунды
let wasRec=false,micInit=false;
const showGain=v=>$('mgainv').textContent='×'+(+v).toFixed(1);
const showNoise=v=>$('mnoisev').textContent=+v?v:'выкл';
async function loadMic(){
  try{
    const r=await(await fetch('/api/mic')).json();
    if(!r.ok){$('recst').textContent='микрофон не запустился';return}
    if(!micInit){$('mgain').value=r.gain;showGain(r.gain);$('mnoise').value=r.noise;showNoise(r.noise);micInit=true}
    $('lvl').style.width=Math.min(100,r.level)+'%';
    $('lvl').style.background=r.level>90?css('--bad'):'';
    $('lvlv').textContent=r.level;
    if(r.rec){$('recst').textContent='● запись '+r.sec.toFixed(1)+' с';$('rec').disabled=$('recafe').disabled=true}
    else if(wasRec){$('recst').textContent='готово';$('rec').disabled=$('recafe').disabled=false;
      const p=$('player');p.src='/mic.wav?'+Date.now();p.hidden=false;p.play().catch(()=>{})}
    wasRec=!!r.rec;
  }catch(e){}
}
$('rec').onclick=()=>fetch('/api/mic?a=rec&s=5').then(()=>{wasRec=true;loadMic()});
$('mgain').oninput=e=>showGain(e.target.value);
$('mgain').onchange=e=>fetch('/api/mic?gain='+e.target.value);
$('mnoise').oninput=e=>showNoise(e.target.value);
$('mnoise').onchange=e=>fetch('/api/mic?noise='+e.target.value);
setInterval(loadMic,300);

// Ассистент
const asText={idle:'Скажи «Alexa» или нажми 🎤',listening:'Слушаю… (нажми ещё раз, чтобы закончить)',thinking:'Думаю…'};
let modelInit=false,thrInit=false;
async function loadAsst(){
  try{
    const r=await(await fetch('/api/assistant')).json();
    $('asst').textContent=asText[r.state]||'';
    $('talk').textContent=r.state==='listening'?'■':'🎤';
    $('talk').classList.toggle('on',r.state!=='idle');
    $('talk').disabled=$('askb').disabled=r.state==='thinking';
    $('heard').textContent=r.heard||'—';
    $('answer').textContent=r.answer||'—';
    $('aserr').textContent=r.error;
    $('gkeyst').textContent=r.key?'Ключ сохранён: '+r.key:'Ключ не задан';
    if(!modelInit){$('gmodel').value=r.model;modelInit=true}
    $('wake').checked=!!r.wake;
    $('aec').checked=!!r.aec;
    if(!thrInit){$('thr').value=r.thr;$('thrv').textContent=(+r.thr).toFixed(2);thrInit=true}
    $('wakest').textContent=(r.live&&r.liveKey?'⚡ Gemini Live · ':'')+r.wakeStatus+(r.wakeCount?' · срабатываний: '+r.wakeCount:'');
    $('liveon').checked=!!r.live;
    $('livest').textContent=(r.liveKey?'Ключ сохранён: '+r.liveKey+' · ':'')+r.liveStatus;
    if(!liveInit){$('livevoice').value=r.liveVoice;$('livemodel').value=r.liveModel;liveInit=true}
  }catch(e){}
}
$('talk').onclick=()=>fetch('/api/assistant?a=talk').then(loadAsst);
$('asstop').onclick=()=>fetch('/api/assistant?a=stop').then(loadAsst);
$('wake').onchange=e=>fetch('/api/assistant?wake='+(e.target.checked?1:0)).then(loadAsst);
$('aec').onchange=e=>fetch('/api/assistant?aec='+(e.target.checked?1:0)).then(loadAsst);
$('thr').oninput=e=>$('thrv').textContent=(+e.target.value).toFixed(2);
$('thr').onchange=e=>fetch('/api/assistant?thr='+e.target.value);
// Запись после обработки — проигрывается тем же плеером, что и тестовая запись микрофона
$('recafe').onclick=()=>fetch('/api/mic?a=recafe&s=5').then(()=>{wasRec=true;loadMic()});
$('askb').onclick=()=>{const q=$('ask').value.trim();if(!q)return;
  fetch('/api/assistant?a=ask&q='+encodeURIComponent(q)).then(()=>{$('ask').value='';loadAsst()})};
$('ask').onkeydown=e=>{if(e.key==='Enter')$('askb').click()};
const groq=p=>fetch('/api/groq',{method:'POST',body:new URLSearchParams(p)});
$('gsave').onclick=()=>{const k=$('gkey').value.trim();if(!k)return;
  groq({key:k}).then(()=>{$('gkey').value='';loadAsst()})};
$('gmsave').onclick=()=>groq({model:$('gmodel').value.trim()}).then(()=>{modelInit=false;loadAsst()});
// Gemini Live
let liveInit=false;
const gemini=p=>fetch('/api/gemini',{method:'POST',body:new URLSearchParams(p)}).then(()=>{liveInit=false;loadAsst()});
$('liveon').onchange=e=>gemini({on:e.target.checked?1:0});
$('livevoice').onchange=e=>gemini({voice:e.target.value});
$('livesave').onclick=()=>{const k=$('livekey').value.trim();if(!k)return;gemini({key:k}).then(()=>$('livekey').value='')};
$('livemsave').onclick=()=>gemini({model:$('livemodel').value.trim()});
// Spotify
function showSpotify(r){
  $('spst').textContent='Состояние: '+r.status+(r.stream?' · играет в колонку через мост, устройство «'+r.device+'»':'');
  if(!$('spstream').value)$('spstream').placeholder=r.stream||'http://192.168.31.X:8000/spotify.mp3';
  $('spredir').textContent=r.redirect;
  if(r.id&&!$('spid').value)$('spid').placeholder='Client ID: '+r.id;
  if(r.device&&!$('spdev').value)$('spdev').placeholder='Устройство: '+r.device;
  const a=$('splogin');
  if(r.authUrl){a.href=r.authUrl;a.style.opacity=1}else{a.removeAttribute('href');a.style.opacity=.5}
}
const spotify=p=>fetch('/api/spotify',p?{method:'POST',body:new URLSearchParams(p)}:{}).then(r=>r.json()).then(showSpotify);
$('spsave').onclick=()=>{const id=$('spid').value.trim(),s=$('spsec').value.trim();if(!id||!s)return;
  spotify({id:id,secret:s}).then(()=>{$('spid').value='';$('spsec').value=''})};
$('spcodesave').onclick=()=>{const c=$('spcode').value.trim();if(!c)return;
  $('spst').textContent='Подключаюсь…';spotify({code:c}).then(()=>$('spcode').value='')};
$('spdevsave').onclick=()=>spotify({device:$('spdev').value.trim()});
$('splogout').onclick=()=>spotify({logout:1});
$('spstreamsave').onclick=()=>spotify({stream:$('spstream').value.trim()}).then(()=>$('spstream').value='');
spotify();

// Telegram: состояние, код привязки, привязанные чаты
function showTg(r){$('tgst').textContent=r.status;$('tgcode').textContent=r.has?r.code:'—';
  const box=$('tgchats');box.innerHTML='';
  for(const c of r.chats){const row=document.createElement('div');row.className='ev';
    const t=document.createElement('span');t.textContent='✓ '+(c.n||c.id);
    const b=document.createElement('button');b.className='btn ghost';b.textContent='Отвязать';
    b.onclick=()=>tg({del:c.id});row.append(t,b);box.append(row)}}
const tg=p=>fetch('/api/telegram',p?{method:'POST',body:new URLSearchParams(p)}:{}).then(r=>r.json()).then(showTg).catch(()=>{});
$('tgsave').onclick=()=>{const t=$('tgtoken').value.trim();if(!t)return;tg({token:t}).then(()=>{$('tgtoken').value='';setTimeout(tg,4000)})};
tg();setInterval(tg,5000);

// Таймеры, будильники, напоминания — список с кнопкой удаления
function showEvents(list){
  const box=$('events');box.innerHTML='';
  if(!list.length){box.innerHTML='<div class="sub" style="margin-top:6px">нет</div>';return}
  for(const e of list){const row=document.createElement('div');row.className='ev';
    const t=document.createElement('span');t.textContent=e.text;
    const b=document.createElement('button');b.className='btn ghost';b.textContent='✕';
    b.onclick=()=>fetch('/api/events?del='+e.id).then(r=>r.json()).then(showEvents);
    row.append(t,b);box.append(row)}
}
const loadEvents=()=>fetch('/api/events').then(r=>r.json()).then(showEvents).catch(()=>{});
setInterval(loadEvents,5000);loadEvents();
setInterval(loadAsst,500);loadAsst();

loadNow();loadHist();setInterval(loadNow,10000);setInterval(loadHist,60000);
</script></body></html>)rawliteral";

// Сброс к заводским (страница /factory)
const char FACTORY_HTML[] PROGMEM = R"rawliteral(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Alexa — сброс</title><style>
body{font-family:system-ui,sans-serif;background:#0b1220;color:#e5e7eb;margin:0;padding:16px}
.c{max-width:460px;margin:0 auto;background:#111a2e;border:1px solid #1f2a44;border-radius:16px;padding:18px}
p,li{color:#94a3b8;font-size:14px}button{width:100%;font-size:16px;padding:12px;border-radius:10px;border:0;
background:#f87171;color:#2a0a0a;font-weight:700;margin-top:10px}#st{color:#e5e7eb}
</style></head><body><div class="c"><h2>⚠ Сброс к заводским настройкам</h2>
<p>Например, перед тем как подарить колонку. Сотрётся:</p>
<ul><li>сеть Wi-Fi и пароль;</li><li>ключи Groq, Google Cloud, Gemini;</li><li>вход в Spotify и адрес моста;</li>
<li>будильники, таймеры, напоминания;</li><li>громкость, голос, чувствительность «Alexa» и другие настройки.</li></ul>
<p><label><input type="checkbox" id="keeptg" checked> Оставить Telegram-бота — чтобы и дальше управлять колонкой издалека</label></p>
<p>Прошивка и история погоды останутся. После сброса колонка включит сеть «Alexa-Setup» — подключись к ней
с телефона и выбери новую сеть Wi-Fi.</p>
<button id="go">Стереть всё и перезагрузить</button><p id="st"></p></div><script>
document.getElementById('go').onclick=()=>{if(!confirm('Точно стереть все настройки и ключи?'))return;
fetch('/factory',{method:'POST',body:new URLSearchParams({keeptg:document.getElementById('keeptg').checked?1:0})}).then(r=>r.text()).then(t=>document.getElementById('st').textContent=t)
.catch(()=>document.getElementById('st').textContent='Колонка перезагружается…')};
</script></body></html>)rawliteral";

const char UPDATE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Прошивка</title>
<style>
:root{--bg:#07120d;--card:#102419;--line:#1f4433;--txt:#e6f4ec;--mut:#8fb5a1;--acc:#34d399;--acc2:#10b981;--accTxt:#03160d}
@media (prefers-color-scheme:light){:root{--bg:#eef8f1;--card:#fff;--line:#cfe7d7;--txt:#0d2a1b;--mut:#4f7562;--acc:#16a34a;--acc2:#15803d;--accTxt:#fff}}
body{margin:0 auto;max-width:600px;padding:16px;background:var(--bg);color:var(--txt);font-family:-apple-system,"Segoe UI",system-ui,sans-serif}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:18px;margin-top:12px}
.mut{color:var(--mut);font-size:13px}
button{background:linear-gradient(135deg,var(--acc),var(--acc2));color:var(--accTxt);border:0;border-radius:12px;padding:10px 18px;font-size:15px;font-weight:600;margin-top:12px;cursor:pointer}
button:disabled{opacity:.5}
progress{width:100%;height:14px;margin-top:12px;accent-color:var(--acc)}
a{color:var(--acc);text-decoration:none}
</style></head><body>
<a href="/">← Назад</a>
<h2>⬆ Обновление прошивки</h2>
<div class="card">
  <div class="mut">Arduino IDE: Скетч → Экспорт скомпилированного бинарного файла.<br>
  Бери файл <b>aht20_bmp280.ino.bin</b> из папки build (не merged.bin).</div>
  <p><input type="file" id="f" accept=".bin"></p>
  <button id="go">Прошить</button>
  <progress id="pr" value="0" max="100" hidden></progress>
  <p id="st"></p>
</div>
<script>
const $=id=>document.getElementById(id);
$('go').onclick=()=>{
  const file=$('f').files[0];
  if(!file){$('st').textContent='Выбери файл .bin';return}
  const fd=new FormData();fd.append('firmware',file,file.name);
  const x=new XMLHttpRequest();
  x.open('POST','/update');
  x.upload.onprogress=e=>{if(e.lengthComputable){$('pr').value=e.loaded/e.total*100;$('st').textContent='Загрузка: '+Math.round(e.loaded/e.total*100)+'%'}};
  x.onload=()=>{
    if(x.status===200){$('st').textContent='Готово! Перезагрузка… страница откроется сама';setTimeout(()=>location.href='/',15000)}
    else{$('st').textContent='Ошибка: '+x.responseText;$('go').disabled=false}
  };
  x.onerror=()=>{$('st').textContent='Связь прервалась';$('go').disabled=false};
  $('go').disabled=true;$('pr').hidden=false;x.send(fd);
};
</script></body></html>)rawliteral";
