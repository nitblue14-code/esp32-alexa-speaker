// Звук: интернет-радио и синтез речи через MAX98357A.
// Вынесено в .h, чтобы препроцессор Arduino не ломал объявления с enum и параметрами по умолчанию.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Audio.h>
#include "gcloud_tts.h"

// Усилитель MAX98357A
#define I2S_BCLK 4
#define I2S_LRC  5
#define I2S_DOUT 6

Audio audio;
uint8_t volume = 8;                 // громкость музыки (радио), 0..21
uint8_t voiceVolume = 18;           // громкость голоса ассистента, 0..21 — своя, не зависит от музыки
float voiceBoostDb = 0.0;           // усиление голоса на плате, -6..+12 дБ (плюс — с мягким ограничителем пиков)
volatile bool audioPlaying = false;
volatile bool speechAudible = false;  // сейчас звучит речь, а не радио — к ней применяется усиление

// Audio работает в отдельной задаче, чтобы звук не заикался, пока ESP32 занята
// веб-страницей или ассистентом. Библиотеку Audio трогаем только из этой задачи,
// остальной код отправляет ей команды через очередь.

enum AudioCmdType : uint8_t { AUDIO_PLAY, AUDIO_SAY, AUDIO_STOP, AUDIO_VOLUME, AUDIO_LIVE };

// Живой разговор (gemini_live.h): ответ приходит кусками в реальном времени. Аудиобиблиотека умеет играть
// только файлы и потоки, поэтому на время разговора она играет бесконечную «тишину» с нашего же сервера,
// а мы прямо перед выводом на усилитель подменяем эту тишину голосом ответа — без лишней задержки.
volatile bool liveAudioOn = false;
void (*livePlaybackHook)(int32_t *buff, int16_t n) = nullptr;  // подставляет голос ответа в выходной звук

struct AudioCmd {
  AudioCmdType type;
  uint8_t vol;
  char arg[320];  // ссылка на поток; текст для озвучки идёт отдельно, через sayBuf
};

QueueHandle_t audioQueue;

// Текст для озвучки может быть длинным (рецепт, сказка) — держим его в PSRAM, а не в очереди команд:
// очередь живёт во внутренней памяти, которой мало
#define SAY_MAX 4096  // ~2000 русских букв
char *sayBuf = nullptr;
SemaphoreHandle_t sayMux;

#define TTS_DEFAULT_VOICE "ru-RU-Chirp3-HD-Leda"
char ttsVoice[48] = TTS_DEFAULT_VOICE;  // голос Google Cloud; "" — простой голос Google Translate

char radioName[120] = "";  // название станции, которая играет
char nowTitle[200] = "";   // что сейчас играет (исполнитель — трек), если станция это передаёт

// Сообщения библиотеки (подключение к потоку, ошибки, название станции) — в Serial Monitor
void audioInfo(Audio::msg_t m) {
  Serial.printf("[audio] %s %s\n", m.s ? m.s : "", m.msg ? m.msg : "");
  if (!m.msg) return;
  if (m.e == Audio::evt_streamtitle) strlcpy(nowTitle, m.msg, sizeof(nowTitle));
  if (m.e == Audio::evt_name && !radioName[0]) strlcpy(radioName, m.msg, sizeof(radioName));
}

// ---------- Усиление голоса ----------
// Google присылает голос почти на пределе громкости, и любое усиление «в файле» обрезает пики — отсюда хрип.
// Поэтому файл берём с запасом, а громче делаем здесь: усиливаем и мягко прижимаем пики (как лимитер
// на радиостанциях) — громко, но без треска. Вызывается аудиобиблиотекой до регулировки громкости.
void audio_process_raw_samples(int32_t *buff, int16_t n) {
  if (liveAudioOn && livePlaybackHook) livePlaybackHook(buff, n);  // живой разговор — тишину меняем на голос
  if (!speechAudible || fabsf(voiceBoostDb) < 0.1f) return;

  static float cachedDb = -100, gain = 1;
  if (voiceBoostDb != cachedDb) {
    cachedDb = voiceBoostDb;
    gain = powf(10.0f, voiceBoostDb / 20.0f);
  }
  // Минус — просто чуть тише, запас от перегруза; ограничитель тут не нужен
  if (voiceBoostDb < 0) {
    for (int i = 0; i < n; i++) buff[i] = (int32_t)(buff[i] * gain);
    return;
  }

  static bool is32 = false;  // отсчёты библиотеки бывают 16-битными или сдвинутыми в старшие биты
  const float T = 0.6f;  // до 60% от максимума — без изменений, выше — плавное сжатие
  for (int i = 0; i < n; i++)
    if (!is32 && abs(buff[i]) > (1 << 20)) is32 = true;
  const float fs = is32 ? 2147483000.0f : 32767.0f;

  for (int i = 0; i < n; i++) {
    float x = buff[i] / fs * gain;
    float a = fabsf(x);
    if (a > T) {
      a = T + (1 - T) * tanhf((a - T) / (1 - T));  // никогда не доходит до 1 — пики не обрезаются
      x = x < 0 ? -a : a;
    }
    buff[i] = (int32_t)(x * fs);
  }
}

// Громкость в библиотеке — своя для речи и для музыки
// Музыка на максимуме перегружает динамик (хрипит выше 18) — поэтому 21 на ползунке теперь = прежние 18.
// Шаги громкости библиотеки — 210, чтобы делить без потерь: голос 0..21 -> 0..210, музыка 0..21 -> 0..180.
#define VOLUME_STEPS     210
#define MUSIC_MAX_LEVEL  18   // хрипит и на 21 — уменьши (например 16)

void audioApplyVolume() {
  audio.setVolume(speechAudible ? voiceVolume * VOLUME_STEPS / 21
                                : (volume * MUSIC_MAX_LEVEL * VOLUME_STEPS + 220) / (21 * 21));
}

// ---------- Речь по предложениям ----------
// Чтобы не ждать синтеза всего ответа, режем его на куски по предложениям. Отдельная задача
// синтезирует кусок N+1, пока играет кусок N, — первый звук появляется примерно через секунду.
// Буферов два — пока один играет, во второй синтезируется следующий кусок.
//
// Звук держим в PSRAM, а не во флеш-памяти: пока идёт запись во флеш, процессор на доли секунды
// останавливает почти всё, включая декодирование звука, — голос трещал и становился неразборчивым.
// Аудиобиблиотека умеет играть из файла или по сети, поэтому плата отдаёт куски речи сама себе
// по сети: маленький сервер на 127.0.0.1:8081 (до внешней сети он не доходит).

#define SPEECH_MAX_CHUNKS 20
#define SPEECH_PORT 8081

enum : int8_t { CHUNK_WAIT, CHUNK_READY, CHUNK_SIMPLE };  // ждёт синтеза / готов файл / говорить простым голосом

struct SpeechChunk {
  String text;
  volatile int8_t state;
};

SpeechChunk speech[SPEECH_MAX_CHUNKS];
volatile int speechCount = 0;
volatile int speechPlayed = 0;    // сколько кусков уже начали играть
volatile uint32_t speechId = 0;   // меняется при новой фразе и при «стоп» — синтез старой фразы бросается
volatile bool speaking = false;
SemaphoreHandle_t speechMux;
TaskHandle_t ttsTask = nullptr;

uint8_t *speechBuf[2] = {nullptr, nullptr};  // звук кусков речи (Ogg Opus 48 кГц), в PSRAM
size_t speechLen[2] = {0, 0};
volatile int8_t servingSlot = -1;            // какой буфер сейчас отдаётся аудиобиблиотеке — его не трогаем

const char *speechUrl(int i) {
  return i % 2 ? "http://127.0.0.1:8081/tts1.ogg" : "http://127.0.0.1:8081/tts0.ogg";
}

// Сервер, который отдаёт аудиобиблиотеке куски речи из памяти
void speechServerTask(void *) {
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));  // сеть нужна даже для 127.0.0.1
  WiFiServer server(SPEECH_PORT);
  server.begin();
  for (;;) {
    WiFiClient c = server.accept();
    if (!c) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    c.setTimeout(2000);
    String request = c.readStringUntil('\n');  // GET /tts0.ogg HTTP/1.1
    while (c.connected()) {                     // остальные заголовки нам не нужны
      String h = c.readStringUntil('\n');
      if (h.length() <= 1) break;
    }
    // Бесконечная тишина для живого разговора (WAV 48 кГц моно) — отдаём, пока разговор идёт
    if (request.indexOf("/silence") >= 0) {
      uint8_t h[44];
      const uint32_t rate = 48000, bytesPerSec = rate * 2, dataLen = 0x7FFF0000;
      memcpy(h, "RIFF", 4);
      uint32_t v = dataLen + 36;  memcpy(h + 4, &v, 4);
      memcpy(h + 8, "WAVEfmt ", 8);
      v = 16;                     memcpy(h + 16, &v, 4);
      uint16_t w = 1;             memcpy(h + 20, &w, 2);  // PCM
      w = 1;                      memcpy(h + 22, &w, 2);  // моно
      memcpy(h + 24, &rate, 4);
      memcpy(h + 28, &bytesPerSec, 4);
      w = 2;                      memcpy(h + 32, &w, 2);
      w = 16;                     memcpy(h + 34, &w, 2);
      memcpy(h + 36, "data", 4);
      memcpy(h + 40, &dataLen, 4);
      // С длиной библиотека считает это файлом и не ищет заново начало звука на каждом куске
      c.printf("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
               (unsigned long)(dataLen + 44));
      c.write(h, sizeof(h));
      // Тишину отдаём в реальном темпе (не больше чем на 0,3 с вперёд). Если лить её без остановки,
      // сетевые буферы съедают внутреннюю память и защищённое соединение с Gemini не открывается.
      static uint8_t zeros[1920] = {0};  // 20 мс
      uint32_t start = millis();
      uint64_t sent = 0;
      while (liveAudioOn && c.connected()) {
        if (sent > (uint64_t)(millis() - start + 300) * bytesPerSec / 1000) {
          vTaskDelay(pdMS_TO_TICKS(10));
          continue;
        }
        size_t n = c.write(zeros, sizeof(zeros));
        if (n) sent += n;
        else vTaskDelay(pdMS_TO_TICKS(5));
      }
      c.stop();
      continue;
    }

    int slot = request.indexOf("/tts1") >= 0 ? 1 : 0;

    // Блокировку держим только чтобы взять буфер: отдача куска идёт, пока он играет, и всё это
    // время синтез следующего куска должен работать. Освободить буфер, пока его отдаём, нельзя — см. servingSlot.
    xSemaphoreTake(speechMux, portMAX_DELAY);
    const uint8_t *buf = speechBuf[slot];
    size_t len = speechLen[slot];
    if (buf) servingSlot = slot;
    xSemaphoreGive(speechMux);

    if (!buf) {
      c.print("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    } else {
      c.printf("HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
               (unsigned)len);
      for (size_t off = 0; off < len;) {
        size_t n = c.write(buf + off, min((size_t)4096, len - off));
        if (!n) break;
        off += n;
      }
    }
    servingSlot = -1;
    c.stop();
  }
}

// Первый кусок — первое предложение (чтобы быстрее начать говорить), дальше — по ~150 символов
int speechSplit(const String &text, String *out, int maxChunks) {
  int n = 0;
  String cur;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    cur += c;
    bool sentenceEnd = (c == '.' || c == '!' || c == '?') && (i + 1 == text.length() || text[i + 1] == ' ');
    bool longEnough = n == 0 ? cur.length() >= 20 : cur.length() >= 150;
    if (sentenceEnd && longEnough && n < maxChunks - 1) {
      cur.trim();
      out[n++] = cur;
      cur = "";
    }
  }
  cur.trim();
  if (cur.length()) out[n++] = cur;
  return n;
}

// Можно ли сейчас говорить красивым голосом
bool ttsCloudUsable() {
  return ttsVoice[0] && !gcloudKey.isEmpty();
}

// После сбоя 5 минут не пробуем Google Cloud, чтобы не ждать на каждой фразе (но новый ключ — пробуем сразу)
uint32_t ttsFailedAt = 0;
String ttsFailedKey;

bool ttsResting() {
  return ttsFailedAt && millis() - ttsFailedAt < 5 * 60 * 1000UL && ttsFailedKey == gcloudKey;
}

void ttsTaskFn(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t id = speechId;
    for (int i = 0;; i++) {
      // Не дальше чем на один кусок вперёд: буфер куска i освобождается, когда начал играть кусок i-1
      while (id == speechId && i < speechCount && i > speechPlayed) vTaskDelay(pdMS_TO_TICKS(20));
      if (id != speechId || i >= speechCount) break;

      xSemaphoreTake(speechMux, portMAX_DELAY);
      String text = speech[i].text;
      xSemaphoreGive(speechMux);

      uint8_t *data = nullptr;
      size_t len = 0;
      bool ok = !ttsResting() && gcloudTtsToMem(text, ttsVoice, data, len);
      if (ok) {
        // Буфер этого слота ещё могут дочитывать — ждём (обычно уже свободен: тот кусок давно доиграл)
        for (int w = 0; servingSlot == i % 2 && w < 500; w++) vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(speechMux, portMAX_DELAY);
        free(speechBuf[i % 2]);
        speechBuf[i % 2] = data;
        speechLen[i % 2] = len;
        xSemaphoreGive(speechMux);
      }
      if (!ok && !ttsResting()) {
        ttsFailedAt = millis();
        ttsFailedKey = gcloudKey;
        Serial.println("[tts] голос Google Cloud не сработал — говорю простым голосом");
      }
      if (ok) ttsFailedAt = 0;
      if (id != speechId) break;
      speech[i].state = ok ? CHUNK_READY : CHUNK_SIMPLE;
    }
    gcloudClose();  // соединение держали открытым только на время одной фразы
  }
}

// Готовим текст к чтению вслух: синтезатор спотыкается о «книжные» знаки и сокращения —
// делает странные паузы, читает «т. е.» по буквам, глотает слова
String speechNormalize(String s) {
  static const char *pairs[][2] = {
    {"т. е.", "то есть"}, {"т.е.", "то есть"},
    {"и т. д.", "и так далее"}, {"и т.д.", "и так далее"},
    {"и т. п.", "и тому подобное"}, {"и т.п.", "и тому подобное"},
    {"т. к.", "так как"}, {"т.к.", "так как"},
    {"и др.", "и другие"}, {"напр.", "например"},
    {"°C", " градусов"}, {"°С", " градусов"}, {"°", " градусов"},
    {"%", " процентов"}, {"№", "номер "}, {"&", " и "}, {"+", " плюс "},
    {" — ", ", "}, {" – ", ", "}, {"—", ", "}, {"–", ", "},
    {"…", "."}, {"...", "."}, {";", ","},
    {"«", ""}, {"»", ""}, {"\"", ""}, {"„", ""}, {"“", ""}, {"”", ""},
    {"(", ", "}, {")", ", "}, {"[", ""}, {"]", ""}, {"*", ""}, {"#", ""}, {"_", " "},
    {"\n", ". "}, {"\r", ""},
  };
  for (auto &p : pairs) s.replace(p[0], p[1]);
  // Подчищаем то, что осталось после замен: двойные пробелы и знаки, пробел перед знаком
  for (int pass = 0; pass < 3; pass++) {
    s.replace("  ", " ");
    s.replace(" ,", ",");
    s.replace(" .", ".");
    s.replace(",,", ",");
    s.replace(",.", ".");
    s.replace("..", ".");
    s.replace(", ,", ",");
  }
  s.trim();
  while (s.startsWith(",") || s.startsWith(".")) {
    s.remove(0, 1);
    s.trim();
  }
  return s;
}

// Начать говорить text (вызывается в аудиозадаче)
void audioStartSpeech(const char *text) {
  audio.stopSong();
  String parts[SPEECH_MAX_CHUNKS];
  int n = speechSplit(speechNormalize(text), parts, SPEECH_MAX_CHUNKS);
  bool cloud = ttsCloudUsable();
  if (!cloud && ttsVoice[0] && gcloudKey.isEmpty())
    strlcpy(ttsStatus, "ключ Google Cloud не задан — говорю простым голосом", sizeof(ttsStatus));

  xSemaphoreTake(speechMux, portMAX_DELAY);
  speechId++;
  for (int i = 0; i < n; i++) {
    speech[i].text = parts[i];
    speech[i].state = cloud ? CHUNK_WAIT : CHUNK_SIMPLE;
  }
  speechCount = n;
  speechPlayed = 0;
  xSemaphoreGive(speechMux);

  speaking = n > 0;
  if (cloud && n) xTaskNotifyGive(ttsTask);
}

// Переключить громкость: речь — своя громкость и усиление, всё остальное — громкость музыки
void audioSetSpeechMode(bool on) {
  if (speechAudible == on) return;
  speechAudible = on;
  audioApplyVolume();
}

void audioStopSpeech() {
  speechId++;
  speaking = false;
  audioSetSpeechMode(false);
}

// Вызывается в цикле аудиозадачи: если предыдущий кусок доиграл и следующий готов — играем его
void audioSpeechLoop() {
  if (!speaking || audio.isRunning()) return;
  int i = speechPlayed;
  if (i >= speechCount) {
    speaking = false;
    audioSetSpeechMode(false);
    gcloudSaveUsage();  // договорили — теперь можно записать счётчик символов во флеш
    return;
  }
  int8_t st = speech[i].state;
  if (st == CHUNK_WAIT) return;  // ещё синтезируется
  audioSetSpeechMode(true);
  if (st == CHUNK_READY) {
    audio.connecttohost(speechUrl(i));
  } else {
    xSemaphoreTake(speechMux, portMAX_DELAY);
    String text = speech[i].text;
    xSemaphoreGive(speechMux);
    audio.connecttospeech(text.c_str(), "ru");
  }
  speechPlayed = i + 1;
}

void audioTask(void *) {
  Audio::audio_info_callback = audioInfo;
  bool pinsOk = audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolumeSteps(VOLUME_STEPS);
  audioApplyVolume();
  // Всегда 48 кГц на выходе — так для эхоподавления (wakeword.h) копию звука легко пересчитать в 16 кГц
  audio.setOutputSampleRate(Audio::SR_48000);
  Serial.printf("[audio] I2S: BCLK=%d LRC=%d DOUT=%d -> %s, громкость %d\n",
                I2S_BCLK, I2S_LRC, I2S_DOUT, pinsOk ? "OK" : "ОШИБКА", volume);

  AudioCmd cmd;
  for (;;) {
    if (xQueueReceive(audioQueue, &cmd, 0) == pdTRUE) {
      bool ok = true;
      switch (cmd.type) {
        case AUDIO_PLAY:   liveAudioOn = false; audioStopSpeech(); nowTitle[0] = 0; ok = audio.connecttohost(cmd.arg); break;
        case AUDIO_SAY: {
          xSemaphoreTake(sayMux, portMAX_DELAY);
          String text = sayBuf ? sayBuf : "";
          xSemaphoreGive(sayMux);
          strlcpy(cmd.arg, text.c_str(), sizeof(cmd.arg));  // только для строки в логе
          liveAudioOn = false;  // «Сказать» с сайта во время живого разговора — разговор заканчивается
          audioStartSpeech(text.c_str());
          break;
        }
        case AUDIO_STOP:   liveAudioOn = false; audioStopSpeech(); audio.stopSong(); break;
        case AUDIO_LIVE:   // начать живой разговор: своя громкость голоса и «тишина», в которую подмешаем ответ
          audioStopSpeech();
          audio.stopSong();
          liveAudioOn = true;
          audioSetSpeechMode(true);
          ok = audio.connecttohost("http://127.0.0.1:8081/silence.wav");
          break;
        case AUDIO_VOLUME: audioApplyVolume(); break;  // громкость музыки или голоса — смотря что звучит
      }
      static const char *names[] = {"play", "say", "stop", "volume", "live"};
      Serial.printf("[audio] команда %s (%s) -> %s\n", names[cmd.type],
                    cmd.type == AUDIO_VOLUME ? String(cmd.vol).c_str() : cmd.arg, ok ? "OK" : "ОШИБКА");
    }
    audioSpeechLoop();
    audio.loop();
    audioPlaying = audio.isRunning() || speaking;  // пока ждём следующий кусок речи — тоже заняты
    vTaskDelay(1);
  }
}

bool sendAudioCmd(AudioCmdType type, const String &arg = "", uint8_t vol = 0) {
  AudioCmd cmd;
  cmd.type = type;
  cmd.vol = vol;
  cmd.arg[0] = 0;
  if (type == AUDIO_SAY) {
    xSemaphoreTake(sayMux, portMAX_DELAY);
    if (sayBuf) strlcpy(sayBuf, arg.c_str(), SAY_MAX);
    xSemaphoreGive(sayMux);
  } else {
    strlcpy(cmd.arg, arg.c_str(), sizeof(cmd.arg));
  }
  return xQueueSend(audioQueue, &cmd, 0) == pdTRUE;
}

void setupAudio() {
  audioQueue = xQueueCreate(4, sizeof(AudioCmd));
  speechMux = xSemaphoreCreateMutex();
  sayMux = xSemaphoreCreateMutex();
  sayBuf = (char *)ps_calloc(SAY_MAX, 1);
  // Стеки — во внутренней памяти, а её же требует каждое защищённое (TLS) соединение. Поэтому
  // ровно столько, сколько нужно: если переборщить, соединения с Groq и Google перестают открываться.
  xTaskCreatePinnedToCore(audioTask, "audio", 12288, nullptr, 2, nullptr, 0);
  // Синтез речи — отдельно, чтобы следующий кусок готовился, пока играет текущий
  xTaskCreatePinnedToCore(ttsTaskFn, "tts", 8192, nullptr, 1, &ttsTask, 1);
  // Отдаёт аудиобиблиотеке куски речи из памяти (см. «Речь по предложениям»)
  xTaskCreatePinnedToCore(speechServerTask, "speechsrv", 4096, nullptr, 1, nullptr, 1);
}

// ---------- Радиостанции ----------
// Один список и для веб-страницы, и для голосовых команд ассистента.

struct Station {
  const char *id;    // короткое имя для ассистента
  const char *name;  // как показывать и называть
  const char *url;
};

const Station STATIONS[] = {
  {"hitfm",  "Хіт FM",              "https://online.hitfm.ua/HitFM"},
  {"kissfm", "Kiss FM",             "https://online.kissfm.ua/KissFM"},
  {"roks",   "Radio ROKS",          "https://online.radioroks.ua/RadioROKS"},
  {"relax",  "Radio Relax",         "https://online.radiorelax.ua/RadioRelax"},
  {"soma",   "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
};
const size_t STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

// Какое радио сейчас должно играть ("" — никакое). Ассистент выключает звук,
// пока слушает и отвечает, а потом по этой ссылке включает радио обратно.
char radioUrl[300] = "";

// Запомнить, какое радио должно играть (включит ассистент, когда договорит)
void radioSet(const String &url, const String &name) {
  strlcpy(radioUrl, url.c_str(), sizeof(radioUrl));
  strlcpy(radioName, name.c_str(), sizeof(radioName));
}

void radioPlay(const String &url, const String &name = "") {
  radioSet(url, name);
  sendAudioCmd(AUDIO_PLAY, url);
}

void radioStop() {
  radioUrl[0] = 0;
  radioName[0] = 0;
  nowTitle[0] = 0;
  sendAudioCmd(AUDIO_STOP);
}

// Громкость музыки (радио)
void audioSetVolume(int v) {
  volume = constrain(v, 0, 21);
  sendAudioCmd(AUDIO_VOLUME, "", volume);
}

// Громкость голоса ассистента — отдельно от музыки
void audioSetVoiceVolume(int v) {
  voiceVolume = constrain(v, 0, 21);
  sendAudioCmd(AUDIO_VOLUME, "", voiceVolume);
}

// Ждём, пока договорит (или доиграет) текущий звук. abort — флаг, по которому ждать перестаём.
void waitAudioDone(uint32_t maxMs = 60000, volatile bool *abort = nullptr) {
  uint32_t t0 = millis();
  auto aborted = [&] { return abort && *abort; };
  while (!audioPlaying && !aborted() && millis() - t0 < 2000) vTaskDelay(pdMS_TO_TICKS(50));  // пока начнёт
  while (audioPlaying && !aborted() && millis() - t0 < maxMs) vTaskDelay(pdMS_TO_TICKS(100));
}

