// Таймеры, будильники и напоминания. Хранятся в памяти платы и переживают перезагрузку.
// Ставит их ассистент по голосовой команде, звонит — assistant.h.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "audio_player.h"  // список станций — для музыки будильника

#define MAX_EVENTS     12
#define EVENT_STALE_S  600  // если плата была выключена и событие пропущено больше чем на 10 минут — не звоним

enum EventType : uint8_t { EV_TIMER, EV_ALARM, EV_REMINDER };
enum EventRepeat : uint8_t { REP_ONCE, REP_DAILY, REP_WEEKDAYS };

struct Event {
  uint16_t id;      // 0 — ячейка свободна
  uint8_t type;     // EventType
  uint8_t repeat;   // EventRepeat (для будильников)
  int32_t seconds;  // для таймера — на сколько поставлен
  int64_t when;     // когда сработать (unix-время)
  char text[96];    // подпись таймера, текст напоминания или музыка будильника: "r:hitfm", "s:artist:Океан Ельзи"
};

extern Preferences prefs;
Event events[MAX_EVENTS];
uint16_t eventNextId = 1;

bool timeValid() {
  return time(nullptr) > 1700000000;  // часы уже синхронизированы с интернетом
}

void eventsSave() {
  prefs.putBytes("events", events, sizeof(events));
  prefs.putUShort("evid", eventNextId);
}

void eventsLoad() {
  if (prefs.getBytesLength("events") == sizeof(events)) prefs.getBytes("events", events, sizeof(events));
  eventNextId = prefs.getUShort("evid", 1);
}

// Ближайший момент hour:minute после after (с учётом «только по будням»)
time_t eventNextAt(int hour, int minute, uint8_t repeat, time_t after) {
  struct tm t;
  localtime_r(&after, &t);
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  for (int i = 0; i < 8; i++) {
    time_t c = mktime(&t);
    struct tm d;
    localtime_r(&c, &d);
    bool dayOk = repeat != REP_WEEKDAYS || (d.tm_wday >= 1 && d.tm_wday <= 5);
    if (c > after && dayOk) return c;
    t.tm_mday += 1;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1;
  }
  return mktime(&t);
}

int eventAdd(uint8_t type, time_t when, uint8_t repeat, int32_t seconds, const String &text) {
  for (Event &e : events) {
    if (e.id) continue;
    e.id = eventNextId++;
    e.type = type;
    e.repeat = repeat;
    e.seconds = seconds;
    e.when = when;
    strlcpy(e.text, text.c_str(), sizeof(e.text));
    eventsSave();
    return e.id;
  }
  return 0;  // все ячейки заняты
}

bool eventCancel(int id) {
  for (Event &e : events) {
    if (e.id && e.id == id) {
      e.id = 0;
      eventsSave();
      return true;
    }
  }
  return false;
}

// type = -1 — отменить всё
int eventCancelAll(int type) {
  int n = 0;
  for (Event &e : events) {
    if (e.id && (type < 0 || e.type == type)) {
      e.id = 0;
      n++;
    }
  }
  if (n) eventsSave();
  return n;
}

String formatDuration(long s) {
  if (s < 0) s = 0;
  long h = s / 3600, m = s % 3600 / 60, sec = s % 60;
  String out;
  if (h) out += String(h) + " ч ";
  if (m || h) out += String(m) + " мин ";
  if (!h) out += String(sec) + " с";
  out.trim();
  return out;
}

String formatClock(time_t t, bool withDate) {
  struct tm d;
  localtime_r(&t, &d);
  char buf[32];
  if (withDate) snprintf(buf, sizeof(buf), "%02d.%02d %02d:%02d", d.tm_mday, d.tm_mon + 1, d.tm_hour, d.tm_min);
  else snprintf(buf, sizeof(buf), "%02d:%02d", d.tm_hour, d.tm_min);
  return buf;
}

// Музыка будильника словами: " с радио Хіт FM", " с музыкой «Океан Ельзи»"
String eventMusicText(const char *m) {
  String s = m;
  if (s.startsWith("r:")) {
    String id = s.substring(2);
    for (size_t i = 0; i < STATION_COUNT; i++)
      if (id == STATIONS[i].id) return String(" с радио ") + STATIONS[i].name;
    return " с радио";
  }
  if (s.startsWith("s:")) return " с музыкой «" + s.substring(s.indexOf(':', 2) + 1) + "»";
  return "";
}

// Описание одного события — для модели и для веб-страницы
String eventDescribe(const Event &e, time_t now) {
  String s;
  if (e.type == EV_TIMER) {
    s = "таймер";
    if (e.text[0]) s += String(" «") + e.text + "»";
    s += " на " + formatDuration(e.seconds) + ", осталось " + formatDuration(e.when - now);
  } else if (e.type == EV_ALARM) {
    s = "будильник на " + formatClock(e.when, false);
    if (e.repeat == REP_DAILY) s += " каждый день";
    else if (e.repeat == REP_WEEKDAYS) s += " по будням";
    else s += " (" + formatClock(e.when, true) + ", один раз)";
    s += eventMusicText(e.text);
  } else {
    s = String("напоминание «") + e.text + "» на " + formatClock(e.when, true);
  }
  return s;
}

String eventsText() {
  time_t now = time(nullptr);
  String s;
  for (const Event &e : events)
    if (e.id) s += "#" + String(e.id) + " " + eventDescribe(e, now) + "; ";
  return s.isEmpty() ? String("нет") : s;
}

// Если какое-то событие пора выполнить — отдаёт его копию; разовые удаляет, повторяющиеся переставляет
bool eventsTakeDue(Event &out) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  for (Event &e : events) {
    if (!e.id || e.when > now) continue;
    bool stale = now - e.when > EVENT_STALE_S;
    out = e;
    if (e.repeat == REP_ONCE) {
      e.id = 0;
    } else {
      time_t w = e.when;
      struct tm t;
      localtime_r(&w, &t);
      e.when = eventNextAt(t.tm_hour, t.tm_min, e.repeat, now);
    }
    eventsSave();
    if (!stale) return true;
  }
  return false;
}
