"""Мост Spotify -> колонка.

Spotify Soloist (официальный клиент Spotify Connect) играет в виртуальный аудиокабель VB-CABLE,
этот скрипт забирает звук с кабеля через ffmpeg, кодирует в MP3 и раздаёт потоком по HTTP:
    http://<адрес ПК>:8000/spotify.mp3
Колонка играет этот поток, как радиостанцию.

Запуск: start_bridge.bat (или: python kolonka_bridge.py). Нужны только стандартные модули Python.
"""

import glob
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))

PORT = 8000
DEVICE_NAME = "Kolonka"         # так колонка видна в Spotify; то же имя — на странице колонки
BITRATE = "192k"
# Громкость без хрипа: компрессор поднимает тихие места, а лимитер не даёт пикам выше LIMIT_DB.
# Усилитель хрипит именно на пиках, поэтому так звучит громче, чем просто убавить на 5 дБ, и не хрипит.
# Хрипит — уменьши LIMIT_DB (например -7); тихо — увеличь MAKEUP_DB.
LIMIT_DB = -5
MAKEUP_DB = 6
CABLE_HINT = "CABLE Output"     # запись с виртуального кабеля VB-CABLE

# Где искать программы: рядом со скриптом, в C:\Users\<вы>\tools или в PATH
TOOLS = os.path.join(os.path.expanduser("~"), "tools")


def find_exe(name, patterns):
    for p in patterns:
        hits = sorted(glob.glob(p, recursive=True))
        if hits:
            return hits[-1]
    return name  # пусть ищет в PATH


FFMPEG = find_exe("ffmpeg", [os.path.join(HERE, "ffmpeg.exe"),
                             os.path.join(TOOLS, "ffmpeg", "**", "ffmpeg.exe")])
SOLOIST = find_exe("soloist", [os.path.join(HERE, "soloist", "soloist.exe"),
                               os.path.join(HERE, "soloist.exe"),
                               os.path.join(TOOLS, "soloist", "**", "soloist.exe")])


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def soloist_key():
    key = os.environ.get("SOLOIST_API_KEY", "").strip()
    path = os.path.join(HERE, "soloist_key.txt")
    if not key and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            key = f.read().strip()
    return key


def cable_device():
    """Имя устройства записи VB-CABLE для ffmpeg (dshow)."""
    out = subprocess.run([FFMPEG, "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace").stderr
    names = re.findall(r'"([^"]+)"\s*\(audio\)', out)
    for n in names:
        if CABLE_HINT.lower() in n.lower():
            return n
    log("Не нашёл устройство записи VB-CABLE. Устройства звука: " + (", ".join(names) or "нет"))
    return None


# ---------- Раздача потока ----------

clients = set()
clients_lock = threading.Lock()


def broadcast(chunk):
    with clients_lock:
        for q in list(clients):
            try:
                q.put_nowait(chunk)
            except queue.Full:  # клиент не успевает (связь плохая) — отключаем, он переподключится
                clients.discard(q)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.split("?")[0] not in ("/spotify.mp3", "/"):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("icy-name", "Spotify")
        self.send_header("Connection", "close")
        self.end_headers()
        q = queue.Queue(maxsize=200)  # ~30 с звука запаса
        with clients_lock:
            clients.add(q)
        log(f"Колонка подключилась: {self.client_address[0]}")
        try:
            while True:
                chunk = q.get(timeout=10)
                self.wfile.write(chunk)
        except Exception:
            pass
        finally:
            with clients_lock:
                clients.discard(q)
            log(f"Колонка отключилась: {self.client_address[0]}")

    def log_message(self, *args):  # без лишнего шума в консоли
        pass


def ffmpeg_loop():
    while True:
        dev = cable_device()
        if not dev:
            time.sleep(10)
            continue
        cmd = [FFMPEG, "-hide_banner", "-loglevel", "error",
               "-f", "dshow", "-audio_buffer_size", "50", "-i", f"audio={dev}",
               "-af", f"acompressor=threshold=-20dB:ratio=3:attack=10:release=200:makeup={MAKEUP_DB}dB,"
                      f"alimiter=limit={10 ** (LIMIT_DB / 20):.3f}:level=disabled",
               "-ac", "2", "-ar", "44100", "-c:a", "libmp3lame", "-b:a", BITRATE,
               "-f", "mp3", "-"]
        log(f"Записываю звук с «{dev}» -> MP3 {BITRATE}, пики до {LIMIT_DB} дБ, подъём {MAKEUP_DB} дБ")
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        while True:
            chunk = p.stdout.read(4096)
            if not chunk:
                break
            broadcast(chunk)
        log("ffmpeg остановился — перезапускаю через 3 с")
        time.sleep(3)


def soloist_loop():
    key = soloist_key()
    if not key:
        log("Нет ключа Soloist: положи его в soloist_key.txt рядом со скриптом. Soloist не запущен.")
        return
    while True:
        log(f"Запускаю Spotify Soloist «{DEVICE_NAME}» ({SOLOIST})")
        # Громкость 100: дальше её регулирует колонка; кэш ограничен, иначе растёт без предела
        code = subprocess.call([SOLOIST, "--device-name", DEVICE_NAME, "--api-key", key,
                                "--initial-volume", "100", "--cache-size", "500"])
        if code == 10:
            log("Сборка Soloist устарела (живёт 90 дней) — скачай новую со страницы Spotify for Developers.")
            return
        log(f"Soloist завершился (код {code}) — перезапуск через 5 с")
        time.sleep(5)


def my_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 80))  # пакеты не отправляются — только выбор сетевой карты
        return s.getsockname()[0]
    except OSError:
        return "адрес-ПК"
    finally:
        s.close()


if __name__ == "__main__":
    log(f"ffmpeg: {FFMPEG}")
    threading.Thread(target=ffmpeg_loop, daemon=True).start()
    if "--no-soloist" not in sys.argv:
        threading.Thread(target=soloist_loop, daemon=True).start()
    log(f"Адрес потока для страницы колонки: http://{my_ip()}:{PORT}/spotify.mp3")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
