// Слово-активатор «Alexa» с эхоподавлением (AEC) — на самой плате, без интернета.
//
// Как это работает:
//   - микрофон слушается постоянно, и каждый отсчёт уходит в AFE (Audio Front End от Espressif);
//   - вместе с ним AFE получает «опорный» сигнал — копию того, что сейчас играет динамик;
//   - AEC вычитает этот звук из микрофона, поэтому нейросеть WakeNet слышит слово даже сквозь радио;
//   - услышали «Alexa» — запускаем ассистента, как будто нажали кнопку.
//
// Модель WakeNet лежит в отдельном разделе памяти "model" (см. partitions.csv в папке скетча)
// и прошивается один раз отдельно — инструкция в README_wakeword.txt.
#pragma once
#include <Arduino.h>
#include <esp_partition.h>
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "audio_player.h"
#include "mic.h"
#include "assistant.h"

// ---------- Опорный сигнал: что сейчас играет динамик ----------
// Аудиобиблиотека отдаёт звук на 48 кГц (стерео, 32 бита), AFE нужен 16 кГц моно, 16 бит.

#define REF_RING        16000  // кольцевой буфер на 1 секунду
#define REF_MAX_LAG     4000   // если опорный сигнал убежал вперёд больше чем на 250 мс — пересинхронизация
#define REF_TARGET_LAG  1400   // ~85 мс — столько звука обычно ждёт в DMA-буфере усилителя

int16_t *refRing = nullptr;
volatile uint32_t refWrite = 0;  // пишет аудиозадача
uint32_t refRead = 0;            // читает задача микрофона

// Вызывается аудиобиблиотекой прямо перед отправкой звука на усилитель
// (это её «слабая» функция — мы подменяем её своей)
void audio_process_i2s(int32_t *outBuff, int16_t validSamples, bool *continueI2S) {
  if (!refRing) return;
  static int32_t acc = 0;
  static int accN = 0;
  static int shift = 16;       // в каком масштабе библиотека держит отсчёты — определяем сами
  static int32_t peak = 0;
  static uint32_t counted = 0;

  for (int i = 0; i + 1 < validSamples; i += 2) {
    int32_t l = outBuff[i], r = outBuff[i + 1];
    int32_t a = abs(l) > abs(r) ? abs(l) : abs(r);
    if (a > peak) peak = a;
    acc += ((l >> shift) + (r >> shift)) / 2;
    if (++accN == 3) {  // 48 кГц -> 16 кГц: среднее из трёх отсчётов
      refRing[refWrite % REF_RING] = constrain(acc / 3, -32768, 32767);
      refWrite++;
      acc = 0;
      accN = 0;
    }
  }
  // Раз в секунду смотрим на пик: отсчёты либо 16-битные, либо сдвинуты в старшие биты
  counted += validSamples;
  if (counted > 96000) {
    if (peak > 0) shift = peak > (1 << 20) ? 16 : 0;
    peak = 0;
    counted = 0;
  }
}

int16_t refNext() {
  uint32_t w = refWrite;
  int32_t lag = (int32_t)(w - refRead);
  if (lag <= 0) return 0;  // ничего не играет
  if (lag > REF_MAX_LAG) refRead = w - REF_TARGET_LAG;
  return refRing[refRead++ % REF_RING];
}

// ---------- AFE + WakeNet ----------

srmodel_list_t *wwModels = nullptr;
esp_afe_sr_iface_t *afe = nullptr;
esp_afe_sr_data_t *afeData = nullptr;
int16_t *afeIn = nullptr;
int afeChunk = 0;
int afeFill = 0;

bool wwEnabled = true;            // можно выключить на странице
bool wwAec = true;                // эхоподавление; можно выключить на странице для проверки
volatile bool wwReady = false;
volatile uint32_t wwDetections = 0;
char wwModelName[40] = "";
char wwStatus[120] = "не запущено";

// Для проверки: пиковые уровни на входе (микрофон, опорный сигнал) и на выходе AFE
volatile int32_t wwPeakMic = 0, wwPeakRef = 0, wwPeakOut = 0;

// Для проверки можно записать то, что после обработки получает WakeNet (флаг micFromAfe в mic.h):
// запись слушается на сайте как обычная тестовая.

// Порог уверенности WakeNet: меньше — срабатывает охотнее (но и чаще ошибается). У модели по умолчанию 0.64.
float wwThreshold = 0.5;  // с одним микрофоном INMP441 при 0.6 слово почти не ловилось, при 0.45–0.5 — уверенно

void wakeSetThreshold(float t) {
  wwThreshold = constrain(t, 0.4f, 0.9f);
  if (afeData) afe->set_wakenet_threshold(afeData, 1, wwThreshold);
}

void wakeStartRecording(int seconds) {
  if (!micBuf || micRecording) return;
  micSamples = 0;
  micTarget = min((size_t)(constrain(seconds, 1, MIC_MAX_SECONDS) * MIC_RATE), micCap);
  micTestRecording = true;  // страница проиграет её как свою тестовую запись
  micFromAfe = true;
  micRecording = true;      // занимаем буфер, чтобы ассистент не писал в него одновременно
}

void wakeSetAec(bool on) {
  wwAec = on;
  if (!afeData) return;
  if (on) afe->enable_aec(afeData);
  else afe->disable_aec(afeData);
}

// Вызывается из задачи микрофона для каждого отсчёта (до шумоподавления)
void wakeFeed(int16_t micSample) {
  if (!wwReady) return;
  int16_t ref = refNext();
  afeIn[afeFill * 2] = micSample;
  afeIn[afeFill * 2 + 1] = ref;
  if (abs(micSample) > wwPeakMic) wwPeakMic = abs(micSample);
  if (abs(ref) > wwPeakRef) wwPeakRef = abs(ref);
  if (++afeFill == afeChunk) {
    afe->feed(afeData, afeIn);
    afeFill = 0;
  }
}

// Пик в процентах от максимума — так же, как индикатор уровня на странице
int wwPct(int32_t peak) {
  return peak * 100 / 32767;
}

void wakeFetchTask(void *) {
  for (;;) {
    afe_fetch_result_t *res = afe->fetch(afeData);
    if (!res || res->ret_value == ESP_FAIL) continue;

    for (int i = 0; i < res->data_size / 2; i++) {
      if (abs(res->data[i]) > wwPeakOut) wwPeakOut = abs(res->data[i]);
      if (micFromAfe && micSamples < micTarget) micBuf[micSamples++] = res->data[i];
    }
    // Живой разговор: Gemini получает звук уже без эха собственного голоса из динамика
    if (liveUplinkHook && liveUplinkFromAfe) liveUplinkHook(res->data, res->data_size / 2);
    if (micFromAfe && micSamples >= micTarget) {
      micFromAfe = false;
      micRecording = false;
    }

    // Раз в 5 секунд — признак жизни и уровни: что пришло на вход и что вышло после обработки
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 5000) {
      lastLog = millis();
      // data_volume считает сам WakeNet: если он всё время ровно 0 — WakeNet не работает
      Serial.printf("[wake] слушаю: микрофон до %d%%, динамик (опорный) до %d%%, после обработки до %d%%, "
                    "WakeNet слышит %.0f дБ, эхоподавление %s%s\n", wwPct(wwPeakMic), wwPct(wwPeakRef),
                    wwPct(wwPeakOut), res->data_volume, wwAec ? "вкл" : "выкл",
                    wwEnabled ? "" : ", слово выключено на сайте");
      wwPeakMic = wwPeakRef = wwPeakOut = 0;
    }
    if (res->wakeup_state != WAKENET_NO_DETECT)
      Serial.printf("[wake] состояние WakeNet: %d, слово №%d\n", (int)res->wakeup_state, res->wake_word_index);
    // Одно слово иногда даёт два срабатывания подряд — второе в течение 2 секунд игнорируем
    static uint32_t lastWake = 0;
    if (res->wakeup_state == WAKENET_DETECTED && millis() - lastWake > 2000) {
      lastWake = millis();
      wwDetections++;
      Serial.printf("[wake] услышал слово-активатор (громкость %.0f дБ)\n", res->data_volume);
      if (wwEnabled && asState == AS_IDLE) assistantTrigger();
    }
  }
}

// Модель лежит в разделе "model". Если раздела нет или он пустой — просто работаем без слова.
bool modelPartitionLooksValid() {
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  if (!part) {
    strlcpy(wwStatus, "нет раздела model — нужен partitions.csv в папке скетча", sizeof(wwStatus));
    return false;
  }
  uint32_t num = 0;
  esp_partition_read(part, 0, &num, sizeof(num));
  if (num == 0 || num > 32) {  // 0xFFFFFFFF — раздел чистый, модель ещё не прошита
    strlcpy(wwStatus, "модель не прошита — см. README_wakeword.txt", sizeof(wwStatus));
    return false;
  }
  return true;
}

bool setupWakeWord() {
  if (!modelPartitionLooksValid()) {
    Serial.printf("[wake] %s\n", wwStatus);
    return false;
  }

  refRing = (int16_t *)ps_calloc(REF_RING, sizeof(int16_t));
  wwModels = esp_srmodel_init("model");
  char *wn = wwModels ? esp_srmodel_filter(wwModels, ESP_WN_PREFIX, NULL) : nullptr;
  if (!wn) {
    strlcpy(wwStatus, "в разделе model нет модели WakeNet", sizeof(wwStatus));
    Serial.printf("[wake] %s\n", wwStatus);
    return false;
  }
  strlcpy(wwModelName, wn, sizeof(wwModelName));

  // "MR": канал 0 — микрофон, канал 1 — опорный сигнал динамика (включает AEC)
  afe_config_t *cfg = afe_config_init("MR", wwModels, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  cfg->aec_init = true;
  cfg->vad_init = false;   // паузы в речи мы ловим сами
  cfg->ns_init = false;
  cfg->agc_init = false;
  cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  // Ядро 0 занято радио и Wi-Fi — распознавание держим на ядре 1, чтобы музыка не заикалась
  cfg->afe_perferred_core = 1;
  cfg->afe_perferred_priority = 3;
  // WakeNet включаем явно: без этого модель загружалась, но слово не распознавалось
  cfg->wakenet_init = true;
  cfg->wakenet_mode = DET_MODE_90;  // обычный режим; чувствительность настраивается порогом на странице
  if (!cfg->wakenet_model_name) cfg->wakenet_model_name = strdup(wn);
  afe_config_print(cfg);  // в лог — что именно включено в обработке звука
  afe = esp_afe_handle_from_config(cfg);
  afeData = afe->create_from_config(cfg);
  afe_config_free(cfg);
  if (afeData) {
    afe->enable_wakenet(afeData);
    afe->print_pipeline(afeData);
  }
  if (!afeData) {
    strlcpy(wwStatus, "не удалось запустить AFE", sizeof(wwStatus));
    Serial.printf("[wake] %s\n", wwStatus);
    return false;
  }

  wakeSetAec(wwAec);
  wakeSetThreshold(wwThreshold);
  afeChunk = afe->get_feed_chunksize(afeData);
  afeIn = (int16_t *)heap_caps_malloc(afeChunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  xTaskCreatePinnedToCore(wakeFetchTask, "wake", 8192, nullptr, 2, nullptr, 1);

  wwReady = true;
  micFeedHook = wakeFeed;
  liveUplinkFromAfe = true;  // для живого разговора берём звук после эхоподавления
  snprintf(wwStatus, sizeof(wwStatus), "работает (%s)", wwModelName);
  Serial.printf("[wake] модель %s, кусок %d отсчётов, эхоподавление включено\n", wwModelName, afeChunk);
  return true;
}
