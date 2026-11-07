#!/usr/bin/env python3
"""
Waybar YTMusic Daemon
Gerencia reprodução de áudio (yt-dlp + ffmpeg + sounddevice),
integração com YouTube Music (ytmusicapi), fila/rádio, biblioteca local/remota
e cache de capas via socket UNIX (/tmp/waybar-ytmusic.sock).
"""

import hashlib
import json
import os
import random
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor, wait
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

import numpy as np
import requests
import sounddevice as sd
import yt_dlp
from PIL import Image, ImageDraw, ImageFont
from ytmusicapi import YTMusic
from ytmusicapi.setup import setup_browser

SOCKET_PATH = "/tmp/waybar-ytmusic.sock"
COVER_CACHE_DIR = Path("/tmp/waybar-ytmusic-covers")
CONFIG_DIR = Path.home() / ".config" / "waybar-ytmusic"
AUTH_FILE = CONFIG_DIR / "browser.json"
LOCAL_LIB_FILE = CONFIG_DIR / "local_library.json"
STATE_FILE = CONFIG_DIR / "player_state.json"

SAMPLE_RATE = 48000
CHANNELS = 2
CHUNK_FRAMES = 2048

_COVER_POOL = ThreadPoolExecutor(max_workers=24)
_HTTP_SESSION = requests.Session()
adapter = requests.adapters.HTTPAdapter(pool_connections=24, pool_maxsize=24)
_HTTP_SESSION.mount("https://", adapter)
_HTTP_SESSION.mount("http://", adapter)
_HTTP_SESSION.headers.update(
    {
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0",
    }
)


def parse_duration_str(dur_str: Optional[str]) -> int:
    if not dur_str:
        return 0
    parts = [int(p) for p in str(dur_str).split(":") if p.isdigit()]
    if len(parts) == 3:
        return parts[0] * 3600 + parts[1] * 60 + parts[2]
    if len(parts) == 2:
        return parts[0] * 60 + parts[1]
    if len(parts) == 1:
        return parts[0]
    return 0


def format_seconds(sec: float) -> str:
    sec = max(0, int(sec))
    m, s = divmod(sec, 60)
    h, m = divmod(m, 60)
    if h > 0:
        return f"{h:d}:{m:02d}:{s:02d}"
    return f"{m:02d}:{s:02d}"


def ensure_default_covers() -> None:
    COVER_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    for size in (44, 96):
        default_path = COVER_CACHE_DIR / f"default_{size}.png"
        if default_path.exists():
            continue
        img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)
        radius = 10 if size >= 80 else 6
        draw.rounded_rectangle(
            (0, 0, size - 1, size - 1),
            radius=radius,
            fill=(30, 30, 46, 255),
            outline=(116, 199, 236, 120),
            width=2 if size >= 80 else 1,
        )
        # Draw simple musical note circle in center
        cx, cy = size // 2, size // 2
        r = size // 6
        draw.ellipse((cx - r - 2, cy, cx + r - 2, cy + 2 * r), fill=(116, 199, 236, 220))
        draw.line((cx + r - 3, cy + r, cx + r - 3, cy - r - 2), fill=(116, 199, 236, 220), width=max(2, size // 20))
        draw.line((cx + r - 3, cy - r - 2, cx + r + 5, cy - r + 2), fill=(116, 199, 236, 220), width=max(2, size // 20))
        img.save(default_path, "PNG")


def _optimize_thumb_url(url: str, size: int) -> str:
    if not url:
        return ""
    target_px = 120 if size <= 60 else 240
    if "googleusercontent.com" in url or "ggpht.com" in url:
        return re.sub(r"=w\d+-h\d+[^&?]*", f"=w{target_px}-h{target_px}-l90-rj", url)
    return url


def _save_cover_bytes(content: bytes, target: Path, size: int) -> bool:
    try:
        from io import BytesIO

        src = Image.open(BytesIO(content)).convert("RGBA")
        w, h = src.size
        min_dim = min(w, h)
        left = (w - min_dim) // 2
        top = (h - min_dim) // 2
        src = src.crop((left, top, left + min_dim, top + min_dim))
        src = src.resize((size, size), Image.Resampling.LANCZOS)

        mask = Image.new("L", (size, size), 0)
        mdraw = ImageDraw.Draw(mask)
        radius = 12 if size >= 80 else 6
        mdraw.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)

        out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        out.paste(src, (0, 0), mask=mask)
        tmp_path = target.with_name(f".{target.name}.{threading.get_ident()}.tmp")
        out.save(tmp_path, "PNG")
        tmp_path.replace(target)
        return True
    except Exception:
        return False


def _download_cover_sync(
    target: Path, url: Optional[str], size: int, fallback_video_id: Optional[str] = None
) -> bool:
    if target.exists() and target.stat().st_size > 0:
        return True

    candidate_urls: List[str] = []
    if url:
        opt = _optimize_thumb_url(url, size)
        if opt:
            candidate_urls.append(opt)
        if url not in candidate_urls:
            candidate_urls.append(url)
    if fallback_video_id and len(fallback_video_id) == 11:
        candidate_urls.append(f"https://i.ytimg.com/vi/{fallback_video_id}/mqdefault.jpg")
        candidate_urls.append(f"https://i.ytimg.com/vi/{fallback_video_id}/hqdefault.jpg")

    for cand in candidate_urls:
        try:
            resp = _HTTP_SESSION.get(cand, timeout=3.5)
            if resp.status_code == 200 and resp.content:
                if _save_cover_bytes(resp.content, target, size):
                    return True
        except Exception:
            continue
    return False


def get_expected_cover_path(item_id: str, size: int = 44) -> str:
    ensure_default_covers()
    if not item_id:
        return str(COVER_CACHE_DIR / f"default_{size}.png")
    safe_id = "".join(c for c in item_id if c.isalnum() or c in ("-", "_"))[:40]
    return str(COVER_CACHE_DIR / f"{safe_id}_{size}.png")


def get_cached_cover(
    item_id: str,
    url: Optional[str],
    size: int = 44,
    async_download: bool = True,
    fallback_video_id: Optional[str] = None,
) -> str:
    ensure_default_covers()
    default_file = str(COVER_CACHE_DIR / f"default_{size}.png")
    if not item_id:
        return default_file
    safe_id = "".join(c for c in item_id if c.isalnum() or c in ("-", "_"))[:40]
    target = COVER_CACHE_DIR / f"{safe_id}_{size}.png"
    if target.exists() and target.stat().st_size > 0:
        return str(target)
    vid = fallback_video_id or (item_id if len(item_id) == 11 else None)
    if not url and not vid:
        return default_file

    if async_download:
        _COVER_POOL.submit(_download_cover_sync, target, url, size, vid)
        return str(target) if (target.exists() and target.stat().st_size > 0) else default_file

    if _download_cover_sync(target, url, size, vid):
        return str(target)
    return default_file


def resolve_covers_for_list(items: List[Dict[str, Any]], timeout: float = 1.8) -> None:
    ensure_default_covers()
    default_file = str(COVER_CACHE_DIR / "default_44.png")
    futures = []
    targets = []
    for it in items:
        item_id = it.get("videoId") or it.get("browseId") or ""
        if not item_id:
            continue
        safe_id = "".join(c for c in item_id if c.isalnum() or c in ("-", "_"))[:40]
        target = COVER_CACHE_DIR / f"{safe_id}_44.png"
        it["expected_cover_small"] = str(target)
        if target.exists() and target.stat().st_size > 0:
            it["cover_small"] = str(target)
            continue
        url = it.get("thumb_small_url") or it.get("thumb_url")
        vid = it.get("videoId")
        if url or vid:
            fut = _COVER_POOL.submit(_download_cover_sync, target, url, 44, vid)
            futures.append(fut)
            targets.append((it, target))
        else:
            it["cover_small"] = default_file

    if futures and timeout > 0:
        wait(futures, timeout=timeout)
    for it, target in targets:
        it["cover_small"] = str(target) if (target.exists() and target.stat().st_size > 0) else default_file


class AudioEngine:
    """Reprodutor de áudio baseado em ffmpeg + sounddevice com suporte a pause, seek e volume."""

    def __init__(self, on_track_end: Callable[[], None]):
        self.on_track_end = on_track_end
        self._lock = threading.RLock()
        self._proc: Optional[subprocess.Popen] = None
        self._pacat_proc: Optional[subprocess.Popen] = None
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._pause_event = threading.Event()  # set = playing, clear = paused
        self._pause_event.set()

        self.state = "stopped"  # stopped | loading | playing | paused
        self.volume = 0.80  # 0.0 .. 1.0
        self.position = 0.0
        self.duration = 0.0
        self._pending_seek: Optional[float] = None
        self.current_stream_url: Optional[str] = None
        self._url_cache: Dict[str, Dict[str, Any]] = {}
        self._generation = 0

    def resolve_stream(self, video_id: str) -> Optional[Dict[str, Any]]:
        now = time.time()
        cached = self._url_cache.get(video_id)
        if cached and (now - cached["ts"] < 1500):
            return cached

        ydl_opts = {
            "format": "bestaudio/best",
            "quiet": True,
            "no_warnings": True,
            "noplaylist": True,
            "extract_flat": False,
        }
        url = f"https://music.youtube.com/watch?v={video_id}"
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            info = ydl.extract_info(url, download=False)
            if not info or not info.get("url"):
                return None
            entry = {
                "url": info["url"],
                "duration": float(info.get("duration") or 0),
                "ts": now,
            }
            self._url_cache[video_id] = entry
            return entry

    def prefetch_video(self, video_id: str) -> None:
        if not video_id or video_id in self._url_cache:
            return

        def _bg():
            try:
                self.resolve_stream(video_id)
            except Exception:
                pass

        threading.Thread(target=_bg, daemon=True).start()

    def play_video(self, video_id: str, known_duration: float = 0.0, start_sec: float = 0.0) -> None:
        with self._lock:
            self._generation += 1
            gen = self._generation
            self._stop_internal()
            self.state = "loading"
            self.position = float(start_sec) if start_sec > 0.0 else 0.0
            self.duration = known_duration
            self._pending_seek = float(start_sec) if start_sec > 0.0 else None

        def _loader():
            try:
                info = self.resolve_stream(video_id)
                if not info:
                    with self._lock:
                        if self._generation == gen:
                            self.state = "stopped"
                    return
                with self._lock:
                    if self._generation != gen:
                        return
                    self.current_stream_url = info["url"]
                    if info["duration"] > 0:
                        self.duration = info["duration"]
                    start_at = self._pending_seek if self._pending_seek is not None else 0.0
                    self._pending_seek = None
                    self._start_pipeline(info["url"], start_at, gen)
            except Exception as e:
                print(f"[AudioEngine] Erro ao carregar {video_id}: {e}", file=sys.stderr)
                with self._lock:
                    if self._generation == gen:
                        self.state = "stopped"

        threading.Thread(target=_loader, daemon=True).start()

    def _start_pipeline(self, stream_url: str, start_sec: float, gen: int) -> None:
        stop_evt = threading.Event()
        self._stop_event = stop_evt
        self._pause_event.set()
        self.position = start_sec
        self.state = "playing"

        cmd = [
            "ffmpeg",
            "-nostdin",
            "-reconnect", "1",
            "-reconnect_streamed", "1",
            "-reconnect_delay_max", "5",
            "-ss", f"{start_sec:.2f}",
            "-i", stream_url,
            "-f", "f32le",
            "-ac", str(CHANNELS),
            "-ar", str(SAMPLE_RATE),
            "-vn",
            "-loglevel", "quiet",
            "pipe:1",
        ]
        self._proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=CHUNK_FRAMES * CHANNELS * 4 * 4,
        )
        self._thread = threading.Thread(
            target=self._audio_loop, args=(self._proc, start_sec, gen, stop_evt), daemon=True
        )
        self._thread.start()

    def _audio_loop(
        self, proc: subprocess.Popen, start_sec: float, gen: int, stop_evt: threading.Event
    ) -> None:
        bytes_per_frame = CHANNELS * 4
        chunk_bytes = CHUNK_FRAMES * bytes_per_frame
        frames_played = 0
        natural_eof = False
        pacat_proc: Optional[subprocess.Popen] = None

        try:
            pacat_cmd = [
                "pacat",
                "--playback",
                "--format=float32le",
                f"--rate={SAMPLE_RATE}",
                f"--channels={CHANNELS}",
                "--client-name=YouTube Music",
                "--stream-name=Waybar YTMusic",
                "--latency-msec=80",
            ]
            pacat_proc = subprocess.Popen(
                pacat_cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                bufsize=chunk_bytes * 2,
            )
            with self._lock:
                if self._generation == gen:
                    self._pacat_proc = pacat_proc

            while not stop_evt.is_set() and self._generation == gen:
                if not self._pause_event.is_set():
                    time.sleep(0.03)
                    continue

                if proc.stdout is None or pacat_proc.stdin is None:
                    break
                raw = proc.stdout.read(chunk_bytes)
                if not raw:
                    if not stop_evt.is_set() and self._generation == gen:
                        natural_eof = True
                    break

                if stop_evt.is_set() or self._generation != gen:
                    break

                valid_bytes = (len(raw) // bytes_per_frame) * bytes_per_frame
                if valid_bytes == 0:
                    continue
                samples = np.frombuffer(raw[:valid_bytes], dtype=np.float32)
                vol = float(self.volume)
                if vol != 1.0:
                    samples = (samples * vol).astype(np.float32)

                pacat_proc.stdin.write(samples.tobytes())
                pacat_proc.stdin.flush()
                frames_played += valid_bytes // bytes_per_frame
                with self._lock:
                    if self._generation == gen and not stop_evt.is_set():
                        self.position = start_sec + (frames_played / float(SAMPLE_RATE))
        except Exception as e:
            if not stop_evt.is_set():
                print(f"[AudioEngine] Loop error: {e}", file=sys.stderr)
        finally:
            if pacat_proc is not None:
                try:
                    if pacat_proc.stdin:
                        pacat_proc.stdin.close()
                    pacat_proc.terminate()
                    pacat_proc.wait(timeout=0.3)
                except Exception:
                    try:
                        pacat_proc.kill()
                    except Exception:
                        pass
            try:
                proc.terminate()
                proc.wait(timeout=0.3)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass

        if natural_eof and self._generation == gen and not stop_evt.is_set():
            with self._lock:
                self.state = "stopped"
            self.on_track_end()

    def _stop_internal(self) -> None:
        self._stop_event.set()
        self._pause_event.set()
        if self._pacat_proc is not None:
            try:
                self._pacat_proc.terminate()
            except Exception:
                pass
            self._pacat_proc = None
        if self._proc is not None:
            try:
                self._proc.terminate()
            except Exception:
                pass
            self._proc = None

    def stop(self) -> None:
        with self._lock:
            self._generation += 1
            self._stop_internal()
            self.state = "stopped"
            self.position = 0.0

    def toggle_pause(self) -> str:
        with self._lock:
            if self.state == "playing":
                self._pause_event.clear()
                self.state = "paused"
            elif self.state == "paused":
                self._pause_event.set()
                self.state = "playing"
            return self.state

    def pause(self) -> None:
        with self._lock:
            if self.state == "playing":
                self._pause_event.clear()
                self.state = "paused"

    def resume(self) -> None:
        with self._lock:
            if self.state == "paused":
                self._pause_event.set()
                self.state = "playing"

    def seek(self, target_sec: float) -> None:
        with self._lock:
            max_dur = self.duration if self.duration > 1.0 else 3600.0
            target_sec = max(0.0, min(float(target_sec), max(0.0, max_dur - 0.5)))
            if self.state == "loading":
                self._pending_seek = target_sec
                self.position = target_sec
                return
            if not self.current_stream_url:
                return
            was_paused = self.state == "paused"
            self._generation += 1
            gen = self._generation
            self._stop_internal()
            self.position = target_sec
            self._start_pipeline(self.current_stream_url, target_sec, gen)
            if was_paused:
                self._pause_event.clear()
                self.state = "paused"

    def set_volume(self, vol_pct: float) -> float:
        vol_pct = max(0.0, min(100.0, float(vol_pct)))
        self.volume = vol_pct / 100.0
        return vol_pct


class YTMusicDaemon:
    def __init__(self):
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        ensure_default_covers()

        self.audio = AudioEngine(on_track_end=self._on_track_finished)
        self.ytm: YTMusic = YTMusic()
        self.authenticated = False
        self.auth_source = "Modo Visitante + Biblioteca Local"

        self.queue: List[Dict[str, Any]] = []
        self.queue_index: int = -1
        self.shuffle_enabled: bool = False
        self.radio_enabled: bool = True
        self.repeat_mode: str = "off"  # off | all | one

        self.local_lib: Dict[str, Any] = {
            "liked_songs": [],
            "history": [],
            "playlists": [],
            "play_counts": {},
            "search_history": [],
        }
        self._home_cache: Optional[Dict[str, Any]] = None
        self._home_cache_ts: float = 0.0

        self._load_local_lib()
        self._init_ytmusic_auth()
        self._load_player_state()
        threading.Thread(target=self.get_home_feed, kwargs={"force_refresh": True}, daemon=True).start()

    def _load_local_lib(self) -> None:
        if LOCAL_LIB_FILE.exists():
            try:
                self.local_lib = json.loads(LOCAL_LIB_FILE.read_text(encoding="utf-8"))
            except Exception:
                pass
        for key in ("liked_songs", "history", "playlists", "search_history"):
            if key not in self.local_lib or not isinstance(self.local_lib[key], list):
                self.local_lib[key] = []

    def _save_local_lib(self) -> None:
        try:
            LOCAL_LIB_FILE.write_text(
                json.dumps(self.local_lib, ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except Exception as e:
            print(f"[Daemon] Erro ao salvar biblioteca local: {e}", file=sys.stderr)

    def _load_player_state(self) -> None:
        self._restored_position = 0.0
        if STATE_FILE.exists():
            try:
                st = json.loads(STATE_FILE.read_text(encoding="utf-8"))
                self.audio.set_volume(st.get("volume", 80))
                self.shuffle_enabled = bool(st.get("shuffle", False))
                self.radio_enabled = bool(st.get("radio", True))
                self.repeat_mode = st.get("repeat", "off")

                raw_q = st.get("queue", [])
                if isinstance(raw_q, list) and raw_q:
                    norm_q = []
                    for item in raw_q:
                        if isinstance(item, dict):
                            n = self._normalize_track(item)
                            if n:
                                norm_q.append(n)
                    if norm_q:
                        self.queue = norm_q
                        saved_idx = int(st.get("queue_index", 0))
                        self.queue_index = max(0, min(saved_idx, len(norm_q) - 1))

                if not self.queue and isinstance(st.get("last_track"), dict):
                    n = self._normalize_track(st["last_track"])
                    if n:
                        self.queue = [n]
                        self.queue_index = 0

                saved_pos = float(st.get("position", 0.0) or 0.0)
                cur_tr = self.current_track()
                if saved_pos > 0.0 and cur_tr:
                    dur = float(cur_tr.get("duration_seconds") or 0)
                    if dur <= 0 or saved_pos < dur - 2.0:
                        self.audio.position = saved_pos
                        self._restored_position = saved_pos
            except Exception as e:
                print(f"[Daemon] Erro ao carregar estado do player: {e}", file=sys.stderr)

        # Fallback: se não houver fila salva ainda, restaura a última música do histórico local
        if not self.queue:
            hist = self.local_lib.get("history", [])
            if isinstance(hist, list) and hist:
                n = self._normalize_track(hist[0])
                if n:
                    self.queue = [n]
                    self.queue_index = 0

        cur = self.current_track()
        if cur:
            self.audio.duration = float(cur.get("duration_seconds") or 0)
            threading.Thread(
                target=get_cached_cover,
                args=(cur["videoId"], cur.get("thumb_url"), 96),
                daemon=True,
            ).start()
            self.audio.prefetch_video(cur["videoId"])

    def _save_player_state(self) -> None:
        try:
            STATE_FILE.write_text(
                json.dumps(
                    {
                        "volume": round(self.audio.volume * 100),
                        "shuffle": self.shuffle_enabled,
                        "radio": self.radio_enabled,
                        "repeat": self.repeat_mode,
                        "queue": self.queue[:60],
                        "queue_index": self.queue_index,
                        "last_track": self.current_track(),
                        "position": round(self.audio.position, 1),
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
        except Exception:
            pass

    def _init_ytmusic_auth(self) -> None:
        if AUTH_FILE.exists():
            try:
                self.ytm = YTMusic(str(AUTH_FILE))
                self.authenticated = True
                self.auth_source = "Conectado ao YouTube Music"
                return
            except Exception as e:
                print(f"[Daemon] Falha ao carregar browser.json: {e}", file=sys.stderr)
        # Tenta importar automaticamente do navegador na inicialização
        res = self.import_browser_auth("auto")
        if res.get("authenticated"):
            return
        self.ytm = YTMusic()
        self.authenticated = False
        self.auth_source = "Modo Visitante (Biblioteca Local Ativa)"

    def import_browser_auth(self, browser_choice: str = "auto") -> Dict[str, Any]:
        """Importa cookies do navegador (Firefox, Zen, Chrome, Chromium, Brave) para autenticar no YTMusic."""
        sqlite_profiles = []
        if browser_choice in ("auto", "firefox", "zen"):
            for base in [
                Path.home() / ".config" / "mozilla" / "firefox",
                Path.home() / ".mozilla" / "firefox",
                Path.home() / ".zen",
                Path.home() / ".config" / "zen",
            ]:
                if base.exists():
                    label = "Zen Browser" if "zen" in str(base) else "Firefox"
                    for db_path in base.rglob("cookies.sqlite"):
                        sqlite_profiles.append((db_path, label))

        last_err = "Nenhum cookie com sessão ativa (__Secure-3PAPISID) encontrado."

        # 1) Try reading Firefox/Zen cookies.sqlite directly (safe copy even when browser is open)
        for db_path, label in sqlite_profiles:
            try:
                with tempfile.TemporaryDirectory() as tmpdir:
                    tmp_db = Path(tmpdir) / "cookies.sqlite"
                    shutil.copy2(db_path, tmp_db)
                    for suffix in ("-wal", "-shm"):
                        wal = Path(str(db_path) + suffix)
                        if wal.exists():
                            shutil.copy2(wal, Path(str(tmp_db) + suffix))
                    conn = sqlite3.connect(str(tmp_db))
                    cur = conn.cursor()
                    cur.execute(
                        "SELECT name, value FROM moz_cookies WHERE host LIKE '%youtube.com%'"
                    )
                    cookies_dict = {name: val for name, val in cur.fetchall()}
                    conn.close()

                if "__Secure-3PAPISID" in cookies_dict:
                    cookie_str = "; ".join(f"{k}={v}" for k, v in cookies_dict.items())
                    raw_headers = (
                        f"cookie: {cookie_str}\n"
                        "authorization: SAPISIDHASH placeholder\n"
                        "x-goog-authuser: 0\n"
                        "origin: https://music.youtube.com\n"
                        "x-origin: https://music.youtube.com\n"
                        "user-agent: Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0\n"
                    )
                    setup_browser(str(AUTH_FILE), raw_headers)
                    self.ytm = YTMusic(str(AUTH_FILE))
                    self.ytm.get_library_playlists(limit=1)
                    self.authenticated = True
                    self.auth_source = f"Conectado via {label}"
                    return {
                        "ok": True,
                        "authenticated": True,
                        "message": f"Conectado com sucesso via {label}!",
                    }
            except Exception as e:
                last_err = str(e)

        # 2) Fallback to yt_dlp extract_cookies_from_browser
        candidates = []
        if browser_choice in ("auto", "firefox"):
            candidates.append(("firefox", None, "Firefox"))
        if browser_choice in ("auto", "chrome"):
            candidates.append(("chrome", None, "Chrome"))
            candidates.append(("chromium", None, "Chromium"))
            candidates.append(("brave", None, "Brave"))

        for b_name, b_profile, label in candidates:
            try:
                jar = yt_dlp.cookies.extract_cookies_from_browser(b_name, profile=b_profile)
                cookies_dict = {}
                for c in jar:
                    if "youtube.com" in c.domain:
                        cookies_dict[c.name] = c.value
                if "__Secure-3PAPISID" in cookies_dict:
                    cookie_str = "; ".join(f"{k}={v}" for k, v in cookies_dict.items())
                    raw_headers = (
                        f"cookie: {cookie_str}\n"
                        "authorization: SAPISIDHASH placeholder\n"
                        "x-goog-authuser: 0\n"
                        "origin: https://music.youtube.com\n"
                        "x-origin: https://music.youtube.com\n"
                        "user-agent: Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0\n"
                    )
                    setup_browser(str(AUTH_FILE), raw_headers)
                    self.ytm = YTMusic(str(AUTH_FILE))
                    self.ytm.get_library_playlists(limit=1)
                    self.authenticated = True
                    self.auth_source = f"Conectado via {label}"
                    return {
                        "ok": True,
                        "authenticated": True,
                        "message": f"Conectado com sucesso via {label}!",
                    }
            except Exception as e:
                last_err = str(e)
                continue

        return {
            "ok": False,
            "authenticated": self.authenticated,
            "message": f"Não foi possível importar automaticamente ({last_err}). Faça login em music.youtube.com no navegador ou cole os Request Headers abaixo.",
        }

    def setup_raw_headers(self, raw_headers: str) -> Dict[str, Any]:
        raw_headers = raw_headers.strip()
        if not raw_headers:
            return {"ok": False, "message": "Cabeçalhos vazios."}
        try:
            # Se o usuário colou apenas o valor do cookie (ex: __Secure-3PAPISID=...)
            if "__Secure-3PAPISID=" in raw_headers and "cookie:" not in raw_headers.lower():
                raw_headers = (
                    f"cookie: {raw_headers}\n"
                    "authorization: SAPISIDHASH placeholder\n"
                    "x-goog-authuser: 0\n"
                    "origin: https://music.youtube.com\n"
                    "x-origin: https://music.youtube.com\n"
                )
            else:
                if "x-goog-authuser" not in raw_headers.lower():
                    raw_headers += "\nx-goog-authuser: 0\norigin: https://music.youtube.com\nx-origin: https://music.youtube.com\n"
                if "authorization:" not in raw_headers.lower():
                    raw_headers += "\nauthorization: SAPISIDHASH placeholder\n"

            setup_browser(str(AUTH_FILE), raw_headers)
            self.ytm = YTMusic(str(AUTH_FILE))
            self.authenticated = True
            self.auth_source = "Conectado ao YouTube Music (Headers)"
            return {"ok": True, "authenticated": True, "message": "Conta conectada com sucesso!"}
        except Exception as e:
            return {"ok": False, "authenticated": self.authenticated, "message": f"Erro ao validar cabeçalhos: {e}"}

    def logout_auth(self) -> Dict[str, Any]:
        if AUTH_FILE.exists():
            AUTH_FILE.unlink()
        self.ytm = YTMusic()
        self.authenticated = False
        self.auth_source = "Modo Visitante (Biblioteca Local Ativa)"
        return {"ok": True, "authenticated": False, "message": "Desconectado da conta remota."}

    def _normalize_track(self, item: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        video_id = item.get("videoId")
        if not video_id:
            return None
        title = item.get("title") or "Sem título"
        artists_list = item.get("artists") or item.get("byline") or []
        if isinstance(artists_list, list):
            artist = ", ".join(a.get("name", "") for a in artists_list if isinstance(a, dict) and a.get("name"))
        elif isinstance(artists_list, str):
            artist = artists_list
        else:
            artist = item.get("artist") or "Artista desconhecido"
        if not artist:
            artist = item.get("artist") or "YouTube Music"

        album_obj = item.get("album")
        if isinstance(album_obj, dict):
            album = album_obj.get("name") or ""
        elif isinstance(album_obj, str):
            album = album_obj
        else:
            album = ""

        duration_sec = item.get("duration_seconds")
        duration_str = item.get("duration") or item.get("length") or ""
        if not duration_sec:
            duration_sec = parse_duration_str(duration_str)
        if not duration_str and duration_sec:
            duration_str = format_seconds(duration_sec)

        thumbs = item.get("thumbnails") or item.get("thumbnail") or []
        if isinstance(thumbs, dict):
            thumbs = thumbs.get("thumbnails") or []
        thumb_url = item.get("thumb_url")
        thumb_small_url = item.get("thumb_small_url")
        if isinstance(thumbs, list) and thumbs:
            if not thumb_url:
                thumb_url = thumbs[-1].get("url")
            if not thumb_small_url:
                thumb_small_url = thumbs[1].get("url") if len(thumbs) > 1 else thumbs[0].get("url")
        if not thumb_small_url:
            thumb_small_url = thumb_url

        liked_ids = {t.get("videoId") for t in self.local_lib.get("liked_songs", [])}
        is_liked = video_id in liked_ids or item.get("likeStatus") == "LIKE"

        cover_small = get_cached_cover(video_id, thumb_small_url, 44, fallback_video_id=video_id)
        expected_cover_small = get_expected_cover_path(video_id, 44)
        return {
            "videoId": video_id,
            "title": title,
            "artist": artist,
            "album": album,
            "duration": duration_str or "0:00",
            "duration_seconds": int(duration_sec or 0),
            "thumb_url": thumb_url or "",
            "thumb_small_url": thumb_small_url or "",
            "cover_small": cover_small,
            "expected_cover_small": expected_cover_small,
            "liked": is_liked,
        }

    def current_track(self) -> Optional[Dict[str, Any]]:
        if 0 <= self.queue_index < len(self.queue):
            return self.queue[self.queue_index]
        return None

    def get_status(self) -> Dict[str, Any]:
        track = self.current_track()
        liked_ids = {t.get("videoId") for t in self.local_lib.get("liked_songs", [])}
        if track:
            track = dict(track)
            track["liked"] = track["videoId"] in liked_ids
            track["cover_small"] = get_cached_cover(
                track["videoId"],
                track.get("thumb_small_url") or track.get("thumb_url"),
                44,
                fallback_video_id=track["videoId"],
            )
            track["cover_large"] = get_cached_cover(
                track["videoId"], track.get("thumb_url"), 96, fallback_video_id=track["videoId"]
            )
            track["expected_cover_large"] = get_expected_cover_path(track["videoId"], 96)

        next_track = None
        if 0 <= self.queue_index + 1 < len(self.queue):
            next_track = self.queue[self.queue_index + 1]

        return {
            "state": self.audio.state,
            "position": round(self.audio.position, 1),
            "duration": round(self.audio.duration or (track["duration_seconds"] if track else 0), 1),
            "volume": round(self.audio.volume * 100),
            "shuffle": self.shuffle_enabled,
            "radio": self.radio_enabled,
            "repeat": self.repeat_mode,
            "authenticated": self.authenticated,
            "auth_source": self.auth_source,
            "track": track,
            "next_track": next_track,
            "queue_length": len(self.queue),
            "queue_index": self.queue_index,
            "default_cover_small": str(COVER_CACHE_DIR / "default_44.png"),
            "default_cover_large": str(COVER_CACHE_DIR / "default_96.png"),
        }

    def _record_history(self, track: Dict[str, Any]) -> None:
        hist = self.local_lib.get("history", [])
        hist = [t for t in hist if t.get("videoId") != track["videoId"]]
        hist.insert(0, track)
        self.local_lib["history"] = hist[:100]
        counts = self.local_lib.get("play_counts")
        if not isinstance(counts, dict):
            counts = {}
        vid = track["videoId"]
        counts[vid] = int(counts.get(vid, 0)) + 1
        self.local_lib["play_counts"] = counts
        self._save_local_lib()

    def _start_current_track(self, start_sec: float = 0.0) -> None:
        track = self.current_track()
        if not track:
            self.audio.stop()
            self._save_player_state()
            return
        self._restored_position = 0.0
        self._record_history(track)
        # Ensure large cover is downloaded in background
        threading.Thread(
            target=get_cached_cover,
            args=(track["videoId"], track.get("thumb_url"), 96),
            daemon=True,
        ).start()
        self.audio.play_video(
            track["videoId"],
            float(track.get("duration_seconds") or 0),
            start_sec=start_sec,
        )
        self._save_player_state()
        # Prefetch next track in queue
        if 0 <= self.queue_index + 1 < len(self.queue):
            self.audio.prefetch_video(self.queue[self.queue_index + 1]["videoId"])

    def _populate_radio_async(self, seed_video_id: str) -> None:
        def _fetch():
            try:
                wp = self.ytm.get_watch_playlist(videoId=seed_video_id, limit=25)
                tracks = wp.get("tracks") or []
                existing_ids = {t["videoId"] for t in self.queue}
                added = 0
                for raw in tracks:
                    norm = self._normalize_track(raw)
                    if norm and norm["videoId"] not in existing_ids:
                        self.queue.append(norm)
                        existing_ids.add(norm["videoId"])
                        added += 1
                if added > 0:
                    self._save_player_state()
                    resolve_covers_for_list(self.queue, timeout=1.5)
                    if 0 <= self.queue_index + 1 < len(self.queue):
                        self.audio.prefetch_video(self.queue[self.queue_index + 1]["videoId"])
            except Exception as e:
                print(f"[Daemon] Erro ao buscar rádio para {seed_video_id}: {e}", file=sys.stderr)

        threading.Thread(target=_fetch, daemon=True).start()

    def play_track(self, track_data: Dict[str, Any], clear_queue: bool = True) -> Dict[str, Any]:
        norm = self._normalize_track(track_data)
        if not norm:
            return {"ok": False, "error": "Faixa inválida"}
        if clear_queue or not self.queue:
            self.queue = [norm]
            self.queue_index = 0
            if self.radio_enabled:
                self._populate_radio_async(norm["videoId"])
        else:
            # Insert right after current track and play
            insert_pos = self.queue_index + 1 if self.queue_index >= 0 else len(self.queue)
            self.queue.insert(insert_pos, norm)
            self.queue_index = insert_pos
        self._start_current_track()
        return self.get_status()

    def play_tracks_list(self, tracks: List[Dict[str, Any]], start_index: int = 0, shuffle: bool = False) -> Dict[str, Any]:
        normalized = []
        for t in tracks:
            n = self._normalize_track(t)
            if n:
                normalized.append(n)
        if not normalized:
            return {"ok": False, "error": "Lista vazia"}
        if shuffle:
            random.shuffle(normalized)
            start_index = 0
        self.queue = normalized
        self.queue_index = max(0, min(start_index, len(normalized) - 1))
        self._start_current_track()
        return self.get_status()

    def add_to_queue(self, track_data: Dict[str, Any], play_next: bool = False) -> Dict[str, Any]:
        norm = self._normalize_track(track_data)
        if not norm:
            return {"ok": False}
        if not self.queue:
            return self.play_track(norm, clear_queue=True)
        if play_next and self.queue_index >= 0:
            self.queue.insert(self.queue_index + 1, norm)
        else:
            self.queue.append(norm)
        if 0 <= self.queue_index + 1 < len(self.queue):
            self.audio.prefetch_video(self.queue[self.queue_index + 1]["videoId"])
        self._save_player_state()
        return {"ok": True, "queue_length": len(self.queue)}

    def play_queue_index(self, idx: int) -> Dict[str, Any]:
        if 0 <= idx < len(self.queue):
            self.queue_index = idx
            self._start_current_track()
        return self.get_status()

    def remove_queue_index(self, idx: int) -> Dict[str, Any]:
        if 0 <= idx < len(self.queue):
            if idx == self.queue_index:
                self.queue.pop(idx)
                if self.queue_index >= len(self.queue):
                    self.queue_index = len(self.queue) - 1
                self._start_current_track()
            else:
                self.queue.pop(idx)
                if idx < self.queue_index:
                    self.queue_index -= 1
                self._save_player_state()
        resolve_covers_for_list(self.queue, timeout=0.6)
        return {"ok": True, "queue": self.queue, "queue_index": self.queue_index}

    def move_queue_index(self, from_idx: int, to_idx: int) -> Dict[str, Any]:
        n = len(self.queue)
        if not (0 <= from_idx < n and 0 <= to_idx < n) or from_idx == to_idx:
            resolve_covers_for_list(self.queue, timeout=0.4)
            return {"ok": True, "queue": self.queue, "queue_index": self.queue_index}

        item = self.queue.pop(from_idx)
        self.queue.insert(to_idx, item)

        if self.queue_index == from_idx:
            self.queue_index = to_idx
        elif from_idx < self.queue_index <= to_idx:
            self.queue_index -= 1
        elif to_idx <= self.queue_index < from_idx:
            self.queue_index += 1

        if 0 <= self.queue_index + 1 < len(self.queue):
            self.audio.prefetch_video(self.queue[self.queue_index + 1]["videoId"])

        self._save_player_state()
        resolve_covers_for_list(self.queue, timeout=0.4)
        return {"ok": True, "queue": self.queue, "queue_index": self.queue_index}

    def next_track(self) -> Dict[str, Any]:
        if not self.queue:
            return self.get_status()
        if self.shuffle_enabled and len(self.queue) > 2:
            choices = [i for i in range(len(self.queue)) if i != self.queue_index]
            self.queue_index = random.choice(choices)
            self._start_current_track()
        elif self.queue_index + 1 < len(self.queue):
            self.queue_index += 1
            self._start_current_track()
            if self.radio_enabled and self.queue_index >= len(self.queue) - 2:
                self._populate_radio_async(self.queue[self.queue_index]["videoId"])
        elif self.repeat_mode == "all":
            self.queue_index = 0
            self._start_current_track()
        else:
            self.audio.stop()
        return self.get_status()

    def prev_track(self) -> Dict[str, Any]:
        if not self.queue:
            return self.get_status()
        if self.audio.position > 4.0:
            self.audio.seek(0.0)
            return self.get_status()
        if self.queue_index > 0:
            self.queue_index -= 1
            self._start_current_track()
        else:
            self.audio.seek(0.0)
        return self.get_status()

    def _on_track_finished(self) -> None:
        if self.repeat_mode == "one" and self.current_track():
            self._start_current_track()
            return
        self.next_track()

    def _record_search_query(self, query: str) -> List[str]:
        q = query.strip()
        if len(q) < 2:
            return self.local_lib.get("search_history", [])[:15]
        hist: List[str] = [s for s in self.local_lib.get("search_history", []) if isinstance(s, str) and s.strip()]
        # If the user is typing progressively (e.g. "daft" -> "daft punk"), replace top prefix
        if hist and (q.lower().startswith(hist[0].lower()) or hist[0].lower().startswith(q.lower())):
            if len(q) >= len(hist[0]):
                hist.pop(0)
            else:
                return hist[:15]
        hist = [s for s in hist if s.lower() != q.lower()]
        hist.insert(0, q)
        self.local_lib["search_history"] = hist[:25]
        self._save_local_lib()
        return self.local_lib["search_history"][:15]

    def get_search_history(self) -> Dict[str, Any]:
        hist = [s for s in self.local_lib.get("search_history", []) if isinstance(s, str) and s.strip()]
        return {"history": hist[:15]}

    def remove_search_history(self, query: str) -> Dict[str, Any]:
        q = query.strip().lower()
        hist = [s for s in self.local_lib.get("search_history", []) if isinstance(s, str) and s.strip().lower() != q]
        self.local_lib["search_history"] = hist
        self._save_local_lib()
        return {"history": hist[:15]}

    def clear_search_history(self) -> Dict[str, Any]:
        self.local_lib["search_history"] = []
        self._save_local_lib()
        return {"history": []}

    def search(self, query: str, filter_type: str = "songs", save_history: bool = True) -> Dict[str, Any]:
        query = query.strip()
        if not query:
            return {
                "items": [],
                "suggestions": [],
                "search_history": self.local_lib.get("search_history", [])[:15],
            }
        if save_history and len(query) >= 2:
            self._record_search_query(query)
        try:
            ytm_filter = filter_type if filter_type in ("songs", "videos", "albums", "playlists", "artists") else "songs"
            sug_future = _COVER_POOL.submit(self.ytm.get_search_suggestions, query)
            raw_results = self.ytm.search(query, filter=ytm_filter, limit=20)
            suggestions: List[str] = []
            try:
                raw_sugs = sug_future.result(timeout=1.5) or []
                for s in raw_sugs:
                    if isinstance(s, str) and s.lower() != query.lower():
                        suggestions.append(s)
                    elif isinstance(s, dict) and s.get("text"):
                        suggestions.append(str(s["text"]))
                suggestions = suggestions[:6]
            except Exception:
                pass

            items = []
            for r in raw_results:
                rtype = r.get("resultType")
                if rtype in ("song", "video") or ytm_filter in ("songs", "videos"):
                    norm = self._normalize_track(r)
                    if norm:
                        norm["item_type"] = "song"
                        items.append(norm)
                elif rtype in ("album", "playlist") or ytm_filter in ("albums", "playlists"):
                    browse_id = r.get("browseId")
                    if not browse_id:
                        continue
                    thumbs = r.get("thumbnails") or []
                    thumb_url = thumbs[-1].get("url") if thumbs else ""
                    thumb_small = thumbs[1].get("url") if len(thumbs) > 1 else thumb_url
                    artists_list = r.get("artists") or []
                    sub = ", ".join(a.get("name", "") for a in artists_list if isinstance(a, dict) and a.get("name"))
                    if not sub:
                        sub = r.get("author") or r.get("year") or rtype.capitalize()
                    items.append(
                        {
                            "item_type": "collection",
                            "collection_type": "album" if ytm_filter == "albums" else "playlist",
                            "browseId": browse_id,
                            "title": r.get("title") or "Sem título",
                            "artist": sub,
                            "duration": r.get("itemCount", "") or r.get("year", ""),
                            "thumb_url": thumb_url,
                            "thumb_small_url": thumb_small,
                            "cover_small": get_cached_cover(browse_id, thumb_small, 44),
                            "expected_cover_small": get_expected_cover_path(browse_id, 44),
                        }
                    )
            resolve_covers_for_list(items, timeout=1.5)
            return {
                "items": items,
                "suggestions": suggestions,
                "search_history": self.local_lib.get("search_history", [])[:15],
            }
        except Exception as e:
            return {
                "items": [],
                "suggestions": [],
                "search_history": self.local_lib.get("search_history", [])[:15],
                "error": str(e),
            }

    def get_collection_tracks(self, browse_id: str, collection_type: str = "playlist") -> Dict[str, Any]:
        try:
            if collection_type == "artist" or browse_id.startswith("UC"):
                art = self.ytm.get_artist(browse_id)
                title = art.get("name") or "Artista"
                songs_sec = art.get("songs") or {}
                songs_pl = songs_sec.get("browseId")
                if songs_pl:
                    clean_pl = songs_pl[2:] if songs_pl.startswith("VL") else songs_pl
                    data = self.ytm.get_playlist(clean_pl, limit=60)
                    raw_tracks = data.get("tracks") or []
                else:
                    raw_tracks = songs_sec.get("results") or []
            elif collection_type == "album" or browse_id.startswith("MPREb_"):
                data = self.ytm.get_album(browse_id)
                title = data.get("title") or "Álbum"
                raw_tracks = data.get("tracks") or []
                thumbs = data.get("thumbnails") or []
                album_thumb = thumbs[-1].get("url") if thumbs else ""
                for t in raw_tracks:
                    if not t.get("thumbnails") and album_thumb:
                        t["thumb_url"] = album_thumb
            else:
                clean_id = browse_id[2:] if browse_id.startswith("VL") else browse_id
                data = self.ytm.get_playlist(clean_id, limit=100)
                title = data.get("title") or "Playlist"
                raw_tracks = data.get("tracks") or []

            tracks = []
            for r in raw_tracks:
                norm = self._normalize_track(r)
                if norm:
                    tracks.append(norm)
            resolve_covers_for_list(tracks, timeout=1.0)
            return {"title": title, "tracks": tracks}
        except Exception as e:
            return {"title": "Erro ao carregar", "tracks": [], "error": str(e)}

    def _normalize_home_item(self, item: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        if not item or not isinstance(item, dict):
            return None
        if item.get("videoId"):
            norm = self._normalize_track(item)
            if norm:
                norm["item_type"] = "song"
                if not norm.get("duration") or norm["duration"] == "0:00":
                    views = item.get("views")
                    if views:
                        norm["duration"] = str(views)
                return norm
        browse_id = item.get("playlistId") or item.get("browseId")
        if browse_id:
            thumbs = item.get("thumbnails") or []
            thumb_url = thumbs[-1].get("url") if thumbs else ""
            if browse_id.startswith("UC"):
                col_type = "artist"
                sub = item.get("subscribers") or "Artista"
            elif browse_id.startswith("MPREb_"):
                col_type = "album"
                artists_list = item.get("artists") or []
                sub = ", ".join(a.get("name", "") for a in artists_list if isinstance(a, dict) and a.get("name")) or "Álbum"
            else:
                col_type = "playlist"
                sub = item.get("description") or item.get("author") or "Mix / Playlist"
                if isinstance(sub, list):
                    sub = "Playlist"
                sub = str(sub).replace("\n", " ")[:55]
            return {
                "item_type": "collection",
                "collection_type": col_type,
                "browseId": browse_id,
                "title": item.get("title") or "Coleção",
                "artist": sub,
                "duration": str(item.get("count") or ""),
                "thumb_url": thumb_url,
                "thumb_small_url": _optimize_thumb_url(thumb_url, 44),
                "cover_small": get_cached_cover(browse_id, thumb_url, 44),
                "expected_cover_small": get_expected_cover_path(browse_id, 44),
            }
        return None

    def get_home_feed(self, force_refresh: bool = False) -> Dict[str, Any]:
        now = time.time()
        if not force_refresh and self._home_cache and (now - self._home_cache_ts < 300):
            cached_items = []
            for s in self._home_cache.get("sections", []):
                cached_items.extend(s.get("items", []))
            resolve_covers_for_list(cached_items, timeout=0.6)
            return self._home_cache

        title_map = {
            "Listen again": "🔁 Ouvir Novamente",
            "Quick picks": "⚡ Escolhas Rápidas (Recomendadas)",
            "Your daily discover": "✨ Sua Descoberta Diária",
            "Covers and remixes": "🎸 Covers e Remixes para Você",
            "From your library": "📚 Da Sua Biblioteca",
            "Forgotten favorites": "💎 Favoritas para Relembrar",
            "Mixed for you": "🎧 Mixes Feitos para Você",
            "Similar to": "🎯 Similares ao que Você Ouve",
            "Recommended music videos": "🎬 Clipes Recomendados",
            "New releases": "🆕 Novos Lançamentos",
        }

        sections: List[Dict[str, Any]] = []
        most_played_tracks: List[Dict[str, Any]] = []
        seen_most: set = set()

        # 1) Compute "Mais Ouvidas por Você" from cloud history + local play counts
        local_counts = self.local_lib.get("play_counts") or {}
        for h in self.local_lib.get("history", [])[:15]:
            norm = self._normalize_track(h)
            if norm and norm["videoId"] not in seen_most:
                norm["item_type"] = "song"
                cnt = int(local_counts.get(norm["videoId"], 1))
                if cnt > 1:
                    norm["duration"] = f"{cnt}x tocada • {norm.get('duration', '')}"
                most_played_tracks.append(norm)
                seen_most.add(norm["videoId"])

        if self.authenticated:
            try:
                cloud_hist = self.ytm.get_history()
                freq: Dict[str, int] = {}
                by_id: Dict[str, Dict[str, Any]] = {}
                order_ids: List[str] = []
                for ch in cloud_hist:
                    vid = ch.get("videoId")
                    if not vid:
                        continue
                    freq[vid] = freq.get(vid, 0) + 1
                    if vid not in by_id:
                        by_id[vid] = ch
                        order_ids.append(vid)
                # Sort by frequency (most played in history first), then recency
                ranked_ids = sorted(order_ids, key=lambda v: (-freq[v], order_ids.index(v)))
                for vid in ranked_ids[:15]:
                    if vid in seen_most:
                        continue
                    norm = self._normalize_track(by_id[vid])
                    if norm:
                        norm["item_type"] = "song"
                        plays = freq[vid] + int(local_counts.get(vid, 0))
                        played_when = by_id[vid].get("played") or ""
                        if plays > 1:
                            norm["duration"] = f"🔥 {plays}x • {norm.get('duration', '')}"
                        elif played_when:
                            norm["duration"] = f"{played_when} • {norm.get('duration', '')}"
                        most_played_tracks.append(norm)
                        seen_most.add(vid)
            except Exception as e:
                print(f"[Daemon] get_history in home error: {e}", file=sys.stderr)

        if most_played_tracks:
            sections.append(
                {
                    "title": "🔥 Mais Ouvidas & Recentes",
                    "items": most_played_tracks[:12],
                }
            )

        # 2) Fetch personalized Home shelves from YouTube Music
        try:
            raw_home = self.ytm.get_home(limit=6)
            for sec in raw_home:
                raw_title = sec.get("title") or "Recomendados"
                pt_title = title_map.get(raw_title, f"🎵 {raw_title}")
                for eng_prefix, pt_val in title_map.items():
                    if raw_title.lower().startswith(eng_prefix.lower()):
                        pt_title = pt_val
                        break
                items = []
                for c in sec.get("contents") or []:
                    norm_item = self._normalize_home_item(c)
                    if norm_item:
                        items.append(norm_item)
                if items:
                    sections.append({"title": pt_title, "items": items[:15]})
        except Exception as e:
            print(f"[Daemon] get_home error: {e}", file=sys.stderr)

        all_home_items = []
        for s in sections:
            all_home_items.extend(s.get("items", []))
        resolve_covers_for_list(all_home_items, timeout=1.2)

        result = {
            "authenticated": self.authenticated,
            "auth_source": self.auth_source,
            "sections": sections,
        }
        self._home_cache = result
        self._home_cache_ts = now
        return result

    def toggle_like(self, track_data: Dict[str, Any]) -> Dict[str, Any]:
        norm = self._normalize_track(track_data)
        if not norm:
            return {"ok": False}
        vid = norm["videoId"]
        liked_list = self.local_lib.get("liked_songs", [])
        exists = any(t.get("videoId") == vid for t in liked_list)
        if exists:
            liked_list = [t for t in liked_list if t.get("videoId") != vid]
            new_state = False
        else:
            norm["liked"] = True
            liked_list.insert(0, norm)
            new_state = True
        self.local_lib["liked_songs"] = liked_list
        self._save_local_lib()

        for q in self.queue:
            if q.get("videoId") == vid:
                q["liked"] = new_state

        if self.authenticated:
            def _remote_rate():
                try:
                    self.ytm.rate_song(vid, "LIKE" if new_state else "INDIFFERENT")
                except Exception:
                    pass
            threading.Thread(target=_remote_rate, daemon=True).start()

        return {"ok": True, "videoId": vid, "liked": new_state}

    def get_liked_songs(self) -> Dict[str, Any]:
        local_liked = list(self.local_lib.get("liked_songs", []))
        seen = {t["videoId"] for t in local_liked if t.get("videoId")}
        for t in local_liked:
            t["liked"] = True
            t["cover_small"] = get_cached_cover(t["videoId"], t.get("thumb_url"), 44)

        if self.authenticated:
            try:
                remote = self.ytm.get_liked_songs(limit=60)
                for r in remote.get("tracks", []):
                    norm = self._normalize_track(r)
                    if norm and norm["videoId"] not in seen:
                        norm["liked"] = True
                        local_liked.append(norm)
                        seen.add(norm["videoId"])
            except Exception as e:
                print(f"[Daemon] get_liked_songs error: {e}", file=sys.stderr)

        resolve_covers_for_list(local_liked, timeout=1.0)
        return {"tracks": local_liked}

    def get_library_overview(self) -> Dict[str, Any]:
        playlists = []
        if self.authenticated:
            try:
                raw_pls = self.ytm.get_library_playlists(limit=40)
                for p in raw_pls:
                    pid = p.get("playlistId")
                    if not pid:
                        continue
                    thumbs = p.get("thumbnails") or []
                    turl = thumbs[-1].get("url") if thumbs else ""
                    count_val = p.get("count") or ""
                    playlists.append(
                        {
                            "item_type": "collection",
                            "collection_type": "playlist",
                            "browseId": pid,
                            "title": p.get("title") or "Playlist",
                            "artist": f"{count_val} faixas" if count_val else "Sua Playlist",
                            "duration": "",
                            "thumb_url": turl,
                            "cover_small": get_cached_cover(pid, turl, 44),
                        }
                    )
            except Exception as e:
                print(f"[Daemon] get_library_playlists error: {e}", file=sys.stderr)

        history = []
        seen_hist = set()
        for h in self.local_lib.get("history", [])[:30]:
            norm = self._normalize_track(h)
            if norm and norm["videoId"] not in seen_hist:
                history.append(norm)
                seen_hist.add(norm["videoId"])

        if self.authenticated:
            try:
                cloud_hist = self.ytm.get_history()
                for ch in cloud_hist[:40]:
                    norm = self._normalize_track(ch)
                    if norm and norm["videoId"] not in seen_hist:
                        history.append(norm)
                        seen_hist.add(norm["videoId"])
            except Exception as e:
                print(f"[Daemon] cloud history error: {e}", file=sys.stderr)

        resolve_covers_for_list(playlists + history, timeout=1.0)
        return {
            "authenticated": self.authenticated,
            "auth_source": self.auth_source,
            "playlists": playlists,
            "history": history,
        }

    def handle_request(self, req: Dict[str, Any]) -> Dict[str, Any]:
        cmd = req.get("cmd", "status")
        if cmd == "status":
            return self.get_status()
        if cmd == "get_home":
            return self.get_home_feed(force_refresh=bool(req.get("refresh", False)))
        if cmd == "toggle":
            if self.audio.state == "stopped" and self.current_track():
                resume_pos = float(getattr(self, "_restored_position", 0.0) or 0.0)
                self._restored_position = 0.0
                self._start_current_track(start_sec=resume_pos)
                if self.radio_enabled and len(self.queue) <= 1:
                    self._populate_radio_async(self.current_track()["videoId"])
            else:
                self.audio.toggle_pause()
                self._save_player_state()
            return self.get_status()
        if cmd == "play":
            if self.audio.state == "stopped" and self.current_track():
                resume_pos = float(getattr(self, "_restored_position", 0.0) or 0.0)
                self._restored_position = 0.0
                self._start_current_track(start_sec=resume_pos)
                if self.radio_enabled and len(self.queue) <= 1:
                    self._populate_radio_async(self.current_track()["videoId"])
            else:
                self.audio.resume()
                self._save_player_state()
            return self.get_status()
        if cmd == "pause":
            self.audio.pause()
            self._save_player_state()
            return self.get_status()
        if cmd == "next":
            return self.next_track()
        if cmd == "prev":
            return self.prev_track()
        if cmd == "seek":
            target_pos = float(req.get("position", 0.0))
            if self.audio.state == "stopped" and self.current_track():
                self.audio.position = target_pos
                self._restored_position = target_pos
            else:
                self.audio.seek(target_pos)
            self._save_player_state()
            return self.get_status()
        if cmd == "volume":
            if "delta" in req:
                new_vol = round(self.audio.volume * 100) + float(req["delta"])
            else:
                new_vol = float(req.get("value", 80))
            self.audio.set_volume(new_vol)
            self._save_player_state()
            return self.get_status()
        if cmd == "toggle_shuffle":
            self.shuffle_enabled = not self.shuffle_enabled
            self._save_player_state()
            return self.get_status()
        if cmd == "toggle_radio":
            self.radio_enabled = not self.radio_enabled
            self._save_player_state()
            return self.get_status()
        if cmd == "toggle_repeat":
            if self.repeat_mode == "off":
                self.repeat_mode = "one"
            elif self.repeat_mode == "one":
                self.repeat_mode = "all"
            else:
                self.repeat_mode = "off"
            self._save_player_state()
            return self.get_status()
        if cmd == "search":
            return self.search(
                req.get("query", ""),
                req.get("filter", "songs"),
                save_history=bool(req.get("save_history", True)),
            )
        if cmd == "get_search_history":
            return self.get_search_history()
        if cmd == "save_search_history":
            return {"history": self._record_search_query(req.get("query", ""))}
        if cmd == "remove_search_history":
            return self.remove_search_history(req.get("query", ""))
        if cmd == "clear_search_history":
            return self.clear_search_history()
        if cmd == "play_track":
            return self.play_track(req.get("track", {}), clear_queue=bool(req.get("clear_queue", True)))
        if cmd == "play_tracks_list":
            return self.play_tracks_list(
                req.get("tracks", []),
                start_index=int(req.get("start_index", 0)),
                shuffle=bool(req.get("shuffle", False)),
            )
        if cmd == "add_to_queue":
            return self.add_to_queue(req.get("track", {}), play_next=bool(req.get("play_next", False)))
        if cmd == "get_queue":
            resolve_covers_for_list(self.queue, timeout=0.8)
            return {"queue": self.queue, "queue_index": self.queue_index}
        if cmd == "play_queue_index":
            return self.play_queue_index(int(req.get("index", 0)))
        if cmd == "remove_queue_index":
            return self.remove_queue_index(int(req.get("index", 0)))
        if cmd == "move_queue_index":
            return self.move_queue_index(int(req.get("from_index", 0)), int(req.get("to_index", 0)))
        if cmd == "clear_queue":
            self.audio.stop()
            self.queue = []
            self.queue_index = -1
            self._save_player_state()
            return self.get_status()
        if cmd == "toggle_like":
            return self.toggle_like(req.get("track", {}))
        if cmd == "get_liked":
            return self.get_liked_songs()
        if cmd == "get_library":
            return self.get_library_overview()
        if cmd == "get_collection":
            return self.get_collection_tracks(
                req.get("browseId", ""), req.get("collection_type", "playlist")
            )
        if cmd == "import_browser_auth":
            return self.import_browser_auth(req.get("browser", "auto"))
        if cmd == "setup_raw_headers":
            return self.setup_raw_headers(req.get("headers", ""))
        if cmd == "logout_auth":
            return self.logout_auth()
        return {"ok": False, "error": f"Comando desconhecido: {cmd}"}


def handle_client(conn: socket.socket, daemon: YTMusicDaemon) -> None:
    try:
        data = b""
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"\n" in chunk:
                break
        if not data:
            return
        req = json.loads(data.decode("utf-8").strip())
        resp = daemon.handle_request(req)
        payload = (json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8")
        conn.sendall(payload)
    except Exception as e:
        err_payload = (json.dumps({"ok": False, "error": str(e)}) + "\n").encode("utf-8")
        try:
            conn.sendall(err_payload)
        except Exception:
            pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def run_server() -> None:
    if os.path.exists(SOCKET_PATH):
        try:
            test_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            test_sock.settimeout(0.5)
            test_sock.connect(SOCKET_PATH)
            test_sock.close()
            print("Daemon já está em execução.")
            return
        except Exception:
            try:
                os.unlink(SOCKET_PATH)
            except Exception:
                pass

    daemon = YTMusicDaemon()
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o600)
    server.listen(16)
    print(f"Waybar YTMusic Daemon ouvindo em {SOCKET_PATH}")

    try:
        while True:
            conn, _ = server.accept()
            threading.Thread(target=handle_client, args=(conn, daemon), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        daemon._save_player_state()
        daemon.audio.stop()
        server.close()
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)


if __name__ == "__main__":
    run_server()
