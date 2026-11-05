// Микрофон INMP441: запись в PSRAM, уровень громкости.
// Вынесено в .h по той же причине, что и audio_player.h.
#pragma once
#include <Arduino.h>
#include <driver/i2s_std.h>

// Микрофон INMP441 (L/R -> GND, значит левый канал)
#define MIC_SCK 15
#define MIC_WS  16
#define MIC_SD  17

#define MIC_RATE        16000   // 16 кГц — стандарт для распознавания речи
#define MIC_MAX_SECONDS 10

// Настраиваются на веб-странице и сохраняются в памяти
volatile float micGain = 4.0f;   // усиление 1..32: тихо — больше, хрипит — меньше
volatile int micNoise = 0;       // шумоподавление 0..100 (0 — выключено)

i2s_chan_handle_t micRx = nullptr;
int16_t *micBuf = nullptr;
const size_t micCap = MIC_RATE * MIC_MAX_SECONDS;
volatile size_t micSamples = 0;    // сколько уже записано
volatile size_t micTarget = 0;     // сколько нужно записать
volatile bool micRecording = false;
volatile int micLevel = 0;         // текущий уровень 0..100
volatile float micNoiseFloor = 3;  // уровень фона в тишине — от него считаем, когда человек замолчал
void (*micFeedHook)(int16_t) = nullptr;  // сюда отдаём каждый отсчёт для слова-активатора (wakeword.h)
volatile bool micFromAfe = false;        // буфер записи сейчас заполняет wakeword.h (что слышит WakeNet), не мы

// Живой разговор (gemini_live.h): куда отдавать звук для Gemini. Лучше — после эхоподавления (его даёт
// wakeword.h, тогда liveUplinkFromAfe = true), иначе — прямо с микрофона отсюда.
void (*liveUplinkHook)(const int16_t *s, int n) = nullptr;
volatile bool liveUplinkFromAfe = false;

// Шумоподавление — это «шумовой затвор»: пока сигнал тише порога (фон, гул компьютера),
// звук приглушается; как только начинаешь говорить — затвор быстро открывается.
// Порог зависит от ползунка: чем больше значение, тем более громкий фон отрезается.
float noiseThreshold() {
  float k = micNoise / 100.0f;
  return k * k * 6000.0f;  // квадрат — чтобы в начале ползунка регулировка была плавнее
}

// Микрофон читается постоянно: так всегда известен уровень,
// а при записи данные дополнительно складываются в буфер.
void micTask(void *) {
  int32_t raw[256];
  float lowpass = 0;     // низкочастотная часть сигнала (постоянка и гул ниже ~80 Гц) — её вычитаем
  float env = 0;         // огибающая громкости — для шумового затвора
  float gate = 1;        // текущее открытие затвора 0..1
  uint32_t holdLeft = 0; // сколько отсчётов держать затвор открытым после речи
  for (;;) {
    size_t bytes = 0;
    if (i2s_channel_read(micRx, raw, sizeof(raw), &bytes, portMAX_DELAY) != ESP_OK) continue;

    float gain = micGain;
    float thr = noiseThreshold();
    int peak = 0;
    for (size_t i = 0; i < bytes / 4; i++) {
      float x = raw[i] / 65536.0f;  // 24 бита в старших разрядах -> масштаб int16
      lowpass += 0.031f * (x - lowpass);
      float y = (x - lowpass) * gain;
      int16_t raw16 = (int16_t)constrain((int)y, -32768, 32767);
      if (micFeedHook) micFeedHook(raw16);  // слову-активатору — без шумового затвора
      if (liveUplinkHook && !liveUplinkFromAfe) liveUplinkHook(&raw16, 1);

      if (thr > 0) {
        float a = fabsf(y);
        env = a > env ? a : env * 0.999f;
        if (env > thr) holdLeft = MIC_RATE / 5;  // держим открытым 200 мс после звука
        else if (holdLeft > 0) holdLeft--;
        float target = holdLeft > 0 ? 1.0f : 0.05f;  // закрытый затвор глушит в 20 раз
        gate += (target - gate) * (target > gate ? 0.01f : 0.0005f);  // открывается быстро, закрывается плавно
        y *= gate;
      }

      int s = constrain((int)y, -32768, 32767);
      if (abs(s) > peak) peak = abs(s);
      if (micRecording && !micFromAfe && micSamples < micTarget) micBuf[micSamples++] = s;
    }
    if (micRecording && !micFromAfe && micSamples >= micTarget) micRecording = false;
    micLevel = peak * 100 / 32767;

    // Фон отслеживаем, пока не пишем: вниз подстраиваемся быстро, вверх — медленно,
    // чтобы случайные звуки не принимались за фон
    if (!micRecording) {
      float k = micLevel < micNoiseFloor ? 0.1f : 0.005f;
      micNoiseFloor += k * (micLevel - micNoiseFloor);
    }
  }
}

void micStopRecording() {
  micRecording = false;
}

volatile bool micTestRecording = false;  // запись запущена кнопкой «Записать» на сайте, а не ассистентом

// test = true — тестовая запись с сайта: только её страница потом проигрывает
void micStartRecording(int seconds, bool test = false) {
  if (!micBuf) return;
  micSamples = 0;
  micTarget = min((size_t)(constrain(seconds, 1, MIC_MAX_SECONDS) * MIC_RATE), micCap);
  micTestRecording = test;
  micRecording = true;
}

bool setupMic() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chanCfg, nullptr, &micRx) != ESP_OK) {
    Serial.println("[mic] не удалось создать I2S-канал");
    return false;
  }

  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_SCK,
      .ws = (gpio_num_t)MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)MIC_SD,
      .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    },
  };
  cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  if (i2s_channel_init_std_mode(micRx, &cfg) != ESP_OK || i2s_channel_enable(micRx) != ESP_OK) {
    Serial.println("[mic] не удалось запустить I2S");
    return false;
  }

  micBuf = (int16_t *)ps_malloc(micCap * sizeof(int16_t));
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 3, nullptr, 1);
  Serial.printf("[mic] INMP441: SCK=%d WS=%d SD=%d, %d Гц -> OK\n", MIC_SCK, MIC_WS, MIC_SD, MIC_RATE);
  return true;
}

// Стандартный 44-байтный заголовок WAV (16 бит, моно)
void micWavHeader(uint8_t *h, uint32_t dataBytes) {
  uint32_t byteRate = MIC_RATE * 2;
  memcpy(h, "RIFF", 4);
  uint32_t v = 36 + dataBytes;  memcpy(h + 4, &v, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  v = 16;                       memcpy(h + 16, &v, 4);
  uint16_t w = 1;               memcpy(h + 20, &w, 2);  // PCM
  w = 1;                        memcpy(h + 22, &w, 2);  // моно
  v = MIC_RATE;                 memcpy(h + 24, &v, 4);
  memcpy(h + 28, &byteRate, 4);
  w = 2;                        memcpy(h + 32, &w, 2);  // байт на отсчёт
  w = 16;                       memcpy(h + 34, &w, 2);  // бит
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &dataBytes, 4);
}
