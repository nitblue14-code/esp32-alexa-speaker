// Журнал колонки на веб-странице: всё, что прошивка пишет в Serial, ещё и копится в кольцевом буфере
// и доступно по адресу http://meteo.local/log — без USB-кабеля и Serial Monitor.
// Подключается первым: дальше в нашем коде Serial — это webLog (пишет и в настоящий порт, и в буфер).
#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>

#define WEBLOG_SIZE (32 * 1024)

class WebLog : public Stream {  // Stream, а не Print: библиотеки (Adafruit) ждут Serial как Stream
 public:
  int available() override { return port().available(); }
  int read() override { return port().read(); }
  int peek() override { return port().peek(); }
  void flush() override { port().flush(); }
  void begin(unsigned long baud) {
    if (!buf) buf = (char *)heap_caps_malloc(WEBLOG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    port().begin(baud);
  }

  size_t write(uint8_t c) override {
    port().write(c);
    put(c);
    return 1;
  }

  size_t write(const uint8_t *buf, size_t n) override {
    port().write(buf, n);
    for (size_t i = 0; i < n; i++) put(buf[i]);
    return n;
  }

  // Последние строки журнала (всё, что есть в буфере)
  String text() {
    String out;
    if (!buf) return out;
    portENTER_CRITICAL(&mux);
    uint32_t w = written, n = w < WEBLOG_SIZE ? w : WEBLOG_SIZE;
    portEXIT_CRITICAL(&mux);
    out.reserve(n);
    for (uint32_t i = w - n; i != w; i++) out += buf[i % WEBLOG_SIZE];
    int nl = n == WEBLOG_SIZE ? out.indexOf('\n') : -1;  // первая строка обрезана — убираем
    return nl >= 0 ? out.substring(nl + 1) : out;
  }

 private:
  char *buf = nullptr;
  volatile uint32_t written = 0;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

  static decltype(::Serial) &port() { return ::Serial; }

  void put(uint8_t c) {
    if (!buf) return;  // буфер выделяется в begin() — до него пишем только в порт
    portENTER_CRITICAL(&mux);
    buf[written % WEBLOG_SIZE] = c;
    written = written + 1;
    portEXIT_CRITICAL(&mux);
  }
};

inline WebLog webLog;
#define Serial webLog
