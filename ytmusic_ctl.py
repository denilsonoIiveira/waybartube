#!/usr/bin/env python3
"""
Waybar YTMusic Controller & Status Script
Script ultraleve (apenas stdlib) usado pelo módulo custom/ytmusic da Waybar
e por atalhos de teclado/mouse.
"""

import html
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

SOCKET_PATH = "/tmp/waybar-ytmusic.sock"
PROJECT_DIR = Path(__file__).resolve().parent
VENV_PYTHON = PROJECT_DIR / ".venv" / "bin" / "python"
DAEMON_SCRIPT = PROJECT_DIR / "ytmusic_daemon.py"
THEME_FILE = Path.home() / ".config" / "waybar-ytmusic" / "theme.json"
WAYBAR_STYLE_FILE = Path.home() / ".config" / "waybar" / "style.css"


def load_theme() -> dict:
    default = {
        "theme": "mocha",
        "accent": "#74c7ec",
        "accent_hover": "#89dceb",
        "accent_rgba": "116, 199, 236",
        "artist_color": "#89b4fa",
        "text_main": "#cdd6f4",
        "text_sub": "#a6adc8",
        "bg_main": "rgba(17, 17, 27, 0.96)",
    }
    if THEME_FILE.exists():
        try:
            data = json.loads(THEME_FILE.read_text(encoding="utf-8"))
            if isinstance(data, dict):
                default.update(data)
        except Exception:
            pass
    return default


def sync_waybar_theme() -> None:
    if not WAYBAR_STYLE_FILE.exists():
        return
    try:
        t = load_theme()
        css = WAYBAR_STYLE_FILE.read_text(encoding="utf-8")
        marker = "#ytmusic {"
        if marker not in css:
            return
        prefix = css.split(marker)[0]
        new_block = f"""#ytmusic {{
\tbackground-color: {t.get("bg_main", "rgba(17, 17, 27, 0.78)")};
\tpadding: 2px 10px;
\tborder-radius: .5rem;
\tborder: 1px solid rgba({t.get("accent_rgba", "116, 199, 236")}, 0.45);
}}

#image.ytmusic-art {{
\tmargin-right: 6px;
}}

#custom-ytmusic-prev,
#custom-ytmusic-play,
#custom-ytmusic-next {{
\tcolor: {t.get("text_main", "#cdd6f4")};
\tpadding: 0 6px;
\tmargin: 0 1px;
\tborder-radius: 6px;
\tfont-size: 13px;
\ttransition: all 0.15s ease;
}}

#custom-ytmusic-prev:hover,
#custom-ytmusic-play:hover,
#custom-ytmusic-next:hover {{
\tbackground-color: {t.get("accent", "#74c7ec")};
\tcolor: #11111b;
}}

#custom-ytmusic-play.playing {{
\tcolor: {t.get("accent", "#74c7ec")};
}}

#custom-ytmusic {{
\tcolor: {t.get("text_main", "#cdd6f4")};
\tpadding: 0 6px 0 8px;
\ttransition: all 0.2s ease;
}}

#custom-ytmusic:hover {{
\tcolor: {t.get("accent_hover", "#89dceb")};
}}

#custom-ytmusic.playing {{
\tcolor: {t.get("text_main", "#cdd6f4")};
}}

#custom-ytmusic.paused {{
\tcolor: #f9e2af;
}}

#custom-ytmusic.loading {{
\tcolor: #f38ba8;
}}
"""
        WAYBAR_STYLE_FILE.write_text(prefix + new_block, encoding="utf-8")
        subprocess.run(["pkill", "-SIGUSR2", "waybar"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def fmt_time(sec: float) -> str:
    sec = max(0, int(sec or 0))
    m, s = divmod(sec, 60)
    return f"{m:02d}:{s:02d}"


def progress_bar(pos: float, dur: float, width: int = 16) -> str:
    if dur <= 0:
        return "─" * width
    ratio = max(0.0, min(1.0, pos / dur))
    filled = int(round(ratio * width))
    if filled >= width:
        return "━" * width
    return ("━" * filled) + "●" + ("─" * max(0, width - filled - 1))


def ensure_daemon_running(wait: bool = False) -> bool:
    if os.path.exists(SOCKET_PATH):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(0.3)
            s.connect(SOCKET_PATH)
            s.close()
            return True
        except Exception:
            pass

    # Start daemon in background
    py_bin = str(VENV_PYTHON) if VENV_PYTHON.exists() else sys.executable
    subprocess.Popen(
        [py_bin, str(DAEMON_SCRIPT)],
        cwd=str(PROJECT_DIR),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    if wait:
        for _ in range(25):
            time.sleep(0.1)
            if os.path.exists(SOCKET_PATH):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.settimeout(0.2)
                    s.connect(SOCKET_PATH)
                    s.close()
                    return True
                except Exception:
                    pass
    return False


def send_cmd(payload: dict, timeout: float = 2.0, auto_start: bool = True) -> dict:
    if not os.path.exists(SOCKET_PATH):
        if auto_start:
            ensure_daemon_running(wait=True)
        else:
            return {}
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(SOCKET_PATH)
        s.sendall((json.dumps(payload) + "\n").encode("utf-8"))
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"\n" in chunk:
                break
        s.close()
        if data:
            return json.loads(data.decode("utf-8"))
    except Exception:
        if auto_start:
            ensure_daemon_running(wait=False)
    return {}


def truncate(text: str, max_len: int = 28) -> str:
    if len(text) <= max_len:
        return text
    return text[: max_len - 1] + "…"


def print_waybar_cover() -> None:
    default_cover = "/tmp/waybar-ytmusic-covers/default_44.png"
    st = send_cmd({"cmd": "status"}, timeout=0.6, auto_start=True)
    if not st or not st.get("track"):
        path = st.get("default_cover_small", default_cover) if st else default_cover
        print(path)
        print("YouTube Music — Clique para abrir")
        return
    track = st.get("track") or {}
    path = track.get("cover_small") or st.get("default_cover_small") or default_cover
    if not os.path.exists(path):
        path = default_cover
    title = track.get("title") or "YouTube Music"
    artist = track.get("artist") or ""
    print(path)
    print(f"{title} • {artist}")


def print_waybar_play_btn() -> None:
    st = send_cmd({"cmd": "status"}, timeout=0.6, auto_start=True)
    state = (st or {}).get("state", "stopped")
    if state == "playing":
        icon = "󰏤"
        cls = "playing"
    elif state == "loading":
        icon = "󱑽"
        cls = "loading"
    else:
        icon = "󰐊"
        cls = "paused" if state == "paused" else "stopped"
    print(
        json.dumps(
            {
                "text": icon,
                "tooltip": "Play / Pause (YouTube Music)",
                "class": cls,
            },
            ensure_ascii=False,
        )
    )


def print_waybar_json() -> None:
    st = send_cmd({"cmd": "status"}, timeout=0.6, auto_start=True)
    th = load_theme()
    acc = th.get("accent", "#74c7ec")
    acc_hov = th.get("accent_hover", "#89dceb")
    art_col = th.get("artist_color", "#89b4fa")
    sub_col = th.get("text_sub", "#a6adc8")

    vol_idle = int((st or {}).get("volume") or 80)
    if not st or not st.get("track"):
        out = {
            "text": f"YTMusic  <span color='#6c7086'>00:00 ────── 00:00</span>  <span color='#f9e2af'>󰕾 {vol_idle}%</span>",
            "tooltip": (
                "<b>󰎆 YouTube Music</b>\n"
                f"<span color='{sub_col}'>Nenhuma música tocando</span>\n\n"
                f"<span color='{acc}'>Clique Esquerdo:</span> Abrir Player / Buscar\n"
                f"<span color='{acc}'>Clique Meio:</span> Play / Pause\n"
                f"<span color='{acc}'>Clique Direito:</span> Próxima faixa\n"
                f"<span color='{acc}'>Scroll:</span> Volume"
            ),
            "class": "stopped",
            "alt": "stopped",
        }
        print(json.dumps(out, ensure_ascii=False))
        return

    state = st.get("state", "stopped")
    track = st.get("track") or {}
    title = track.get("title") or "Sem título"
    artist = track.get("artist") or "YouTube Music"
    album = track.get("album") or ""
    pos = float(st.get("position") or 0)
    dur = float(st.get("duration") or 0)
    vol = int(st.get("volume") or 80)
    liked = bool(track.get("liked"))
    next_track = st.get("next_track")

    if state == "loading":
        css_class = "loading"
    elif state == "playing":
        css_class = "playing"
    elif state == "paused":
        css_class = "paused"
    else:
        css_class = "stopped"

    heart = " <span color='#f38ba8'>♥</span>" if liked else ""
    short_label = html.escape(truncate(f"{title} • {artist}", 26))
    inline_bar = progress_bar(pos, dur, 8)
    timeline_str = f"<span color='{acc_hov}'>{fmt_time(pos)} {inline_bar} {fmt_time(dur)}</span>"
    vol_str = f"<span color='#f9e2af'>󰕾 {vol}%</span>"
    bar_text = f"{short_label}{heart}  {timeline_str}  {vol_str}"

    pbar = progress_bar(pos, dur, 18)
    tooltip_lines = [
        f"<b>󰎆 {html.escape(title)}</b>",
        f"<span color='{art_col}'>󰠃 {html.escape(artist)}</span>",
    ]
    if album:
        tooltip_lines.append(f"<span color='{sub_col}'>󰀥 {html.escape(album)}</span>")
    tooltip_lines.append("")
    tooltip_lines.append(
        f"<span color='{acc}'>{fmt_time(pos)}</span>  {pbar}  <span color='{acc}'>{fmt_time(dur)}</span>"
    )
    tooltip_lines.append(f"<span color='#f9e2af'>󰕾 Volume: {vol}%</span>")
    if next_track:
        nt_title = html.escape(truncate(next_track.get("title", ""), 30))
        nt_artist = html.escape(truncate(next_track.get("artist", ""), 20))
        tooltip_lines.append(f"\n<span color='{sub_col}'>󰒭 A seguir: {nt_title} ({nt_artist})</span>")

    out = {
        "text": bar_text,
        "tooltip": "\n".join(tooltip_lines),
        "class": css_class,
        "alt": state,
    }
    print(json.dumps(out, ensure_ascii=False))


def handle_media_key(cmd_name: str, playerctl_arg: str) -> None:
    st = send_cmd({"cmd": "status"}, timeout=0.4, auto_start=False)
    if st and st.get("track"):
        send_cmd({"cmd": cmd_name}, timeout=1.0, auto_start=False)
    else:
        subprocess.run(["playerctl", playerctl_arg], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main() -> None:
    action = sys.argv[1] if len(sys.argv) > 1 else "waybar"
    if action == "waybar":
        print_waybar_json()
    elif action == "waybar-cover":
        print_waybar_cover()
    elif action == "waybar-play":
        print_waybar_play_btn()
    elif action == "toggle":
        send_cmd({"cmd": "toggle"})
    elif action == "next":
        send_cmd({"cmd": "next"})
    elif action == "prev":
        send_cmd({"cmd": "prev"})
    elif action == "media-toggle":
        handle_media_key("toggle", "play-pause")
    elif action == "media-next":
        handle_media_key("next", "next")
    elif action == "media-prev":
        handle_media_key("prev", "previous")
    elif action == "vol-up":
        send_cmd({"cmd": "volume", "delta": 5})
    elif action == "vol-down":
        send_cmd({"cmd": "volume", "delta": -5})
    elif action == "status":
        print(json.dumps(send_cmd({"cmd": "status"}), indent=2, ensure_ascii=False))
    elif action == "start-daemon":
        ensure_daemon_running(wait=True)
    elif action == "sync-waybar-theme":
        sync_waybar_theme()


if __name__ == "__main__":
    main()
