# 󰎆 WaybarTube (`waybar-ytmusic`)

Cliente nativo de **YouTube Music** para **Waybar** (Wayland / Hyprland), desenvolvido em **C (GTK3 + `gtk-layer-shell`)** com daemon de áudio e sincronização em **Python (`ytmusicapi` + `yt-dlp` + `ffmpeg` + `pacat`)**.

---

## ✨ Funcionalidades

- **Integração Completa na Barra da Waybar**:
  - Exibe a **miniatura da capa da música** (`image#ytmusic-art`), botões de mídia (`󰒮`, `󰐊/󰏤`, `󰒭`), **timeline ao vivo** (`01:15 ━━━●──── 03:20`) e **volume** diretamente na barra.
- **Mini-Player Compacto e Expansível**:
  - Ao clicar no módulo da Waybar, abre um **Mini-Player compacto** com capa, controles, timeline interativa, modo aleatório (`󰒝`), repetição (`󰑖` Desligada / `󰑘 1` Música / `󰑖 All` Fila), curtir (`♥`) e rádio automática (`󰐹 Rádio`).
  - Clicar no card da música ou em **`󰁝 Expandir`** abre a interface completa (indo direto para a **`󰲸 Fila`** quando há música em reprodução).
  - **Clicar fora do player** fecha/minimiza automaticamente a janela de volta para a Waybar.
- **Abas Completas**:
  - **`󰋜 Início`**: Recomendações personalizadas, faixas mais ouvidas por você e descobertas.
  - **`󰍉 Buscar`**: Busca em tempo real enquanto você digita, pílulas de sugestões de autocompletar, filtros por *Músicas*, *Álbuns* e *Playlists*, e **histórico de últimas buscas** (`🕒 Últimas buscas`).
  - **`󰲸 Fila`**:
    - **Clique simples** na música para tocar imediatamente.
    - **Arrastar para cima/baixo (Drag & Drop)** para reordenar a posição na fila.
    - **Arrastar para a direita (Swipe Right)** para remover a música da fila com animação vermelha.
  - **`♥ Curtidas`** e **`󰉋 Biblioteca`**: Suas músicas curtidas, playlists da conta e histórico de reprodução.
  - **`󰀄 Conta`**: Importação automática de sessão/cookies do navegador (*Firefox*, *Zen Browser*, *Chrome*, *Brave*, *Chromium*) com 1 clique ou colagem manual de *Request Headers*.
- **Persistência Automática**:
  - A última música, posição da timeline, fila de reprodução, volume e modo de repetição ficam salvos em `~/.config/waybar-ytmusic/player_state.json`.
- **Temas de Cores (8 Paletas Prontas + Tema Customizado)**:
  - Seletor em tempo real na aba **`󰏘 Config`** com sincronização automática das cores no popup e na Waybar (`~/.config/waybar-ytmusic/theme.json`).

---

## 📦 1. Dependências do Sistema

No **Arch Linux / CachyOS / Manjaro**, instale os pacotes necessários:

```bash
sudo pacman -S --needed base-devel pkgconf gtk3 gtk-layer-shell json-glib ffmpeg libpulse python uv playerctl
```

*(Em distribuições baseadas em Debian/Ubuntu: `sudo apt install build-essential pkg-config libgtk-3-dev libgtk-layer-shell-dev libjson-glib-dev ffmpeg pulseaudio-utils python3 python3-venv playerctl`)*

---

## 🚀 2. Instalação e Compilação

1. **Clone o repositório:**

```bash
git clone git@github.com:denilsonoIiveira/waybartube.git ~/dev/waybar-ytmusic
cd ~/dev/waybar-ytmusic
```

2. **Crie o ambiente Python e instale as dependências:**

Com **`uv`** (recomendado):
```bash
uv sync
```

Ou usando `venv` + `pip` padrão:
```bash
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install ytmusicapi yt-dlp pillow numpy requests
```

3. **Compile a interface nativa em C (`ytmusic-popup`):**

```bash
make
chmod +x ytmusic_ctl.py ytmusic_daemon.py toggle-ytmusic.sh
```

---

## 📊 3. Configuração na Waybar

### 3.1. `config.jsonc`
Adicione o grupo `"group/ytmusic"` na sua lista de módulos (ex.: `modules-left`):

```jsonc
"modules-left": ["hyprland/workspaces", "group/ytmusic"],

"group/ytmusic": {
    "orientation": "horizontal",
    "modules": [
        "image#ytmusic-art",
        "custom/ytmusic-prev",
        "custom/ytmusic-play",
        "custom/ytmusic-next",
        "custom/ytmusic"
    ]
}
```

### 3.2. `modules.jsonc`
Adicione as definições dos módulos apontando para o caminho onde você clonou o projeto (ajuste `/seu/caminho/waybar-ytmusic` ou `~/dev/waybar-ytmusic`):

```jsonc
"image#ytmusic-art": {
    "exec": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py waybar-cover",
    "size": 22,
    "interval": 2,
    "on-click": "/home/SEU_USUARIO/dev/waybar-ytmusic/toggle-ytmusic.sh",
    "on-scroll-up": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py vol-up",
    "on-scroll-down": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py vol-down",
    "tooltip": true
},

"custom/ytmusic-prev": {
    "format": "󰒮",
    "on-click": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py prev",
    "tooltip": true,
    "tooltip-format": "Faixa anterior"
},

"custom/ytmusic-play": {
    "format": "{}",
    "return-type": "json",
    "interval": 1,
    "exec": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py waybar-play",
    "on-click": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py toggle",
    "tooltip": true
},

"custom/ytmusic-next": {
    "format": "󰒭",
    "on-click": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py next",
    "tooltip": true,
    "tooltip-format": "Próxima faixa"
},

"custom/ytmusic": {
    "format": "{}",
    "return-type": "json",
    "interval": 1,
    "exec": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py waybar",
    "on-click": "/home/SEU_USUARIO/dev/waybar-ytmusic/toggle-ytmusic.sh",
    "on-click-middle": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py toggle",
    "on-click-right": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py next",
    "on-scroll-up": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py vol-up",
    "on-scroll-down": "/home/SEU_USUARIO/dev/waybar-ytmusic/ytmusic_ctl.py vol-down",
    "tooltip": true,
    "escape": false
}
```

### 3.3. `style.css`
Adicione o estilo do grupo no seu `~/.config/waybar/style.css`:

```css
#ytmusic {
    background-color: #11111b;
    border-radius: 10px;
    padding: 2px 10px;
    margin-left: 8px;
    border: 1px solid rgba(116, 199, 236, 0.35);
}

#ytmusic-art {
    margin-right: 6px;
    border-radius: 4px;
}

#custom-ytmusic-prev,
#custom-ytmusic-play,
#custom-ytmusic-next {
    color: #cdd6f4;
    padding: 0 5px;
    font-size: 14px;
}

#custom-ytmusic-play.playing {
    color: #a6e3a1;
}

#custom-ytmusic {
    color: #cdd6f4;
    padding-left: 6px;
    font-weight: bold;
}

#custom-ytmusic.playing {
    color: #74c7ec;
}
```

Recarregue a Waybar:
```bash
killall -SIGUSR2 waybar
```

---

## ⌨️ 4. Atalhos de Mídia do Teclado (Hyprland)

Para que as teclas multimídia do teclado (`Play/Pause`, `Next`, `Prev`) controlem o **WaybarTube** (com fallback automático para o `playerctl` quando nenhuma música estiver carregada), adicione ao seu `hyprland.conf`:

```ini
bindl = , XF86AudioPlay,  exec, ~/dev/waybar-ytmusic/ytmusic_ctl.py media-toggle
bindl = , XF86AudioPause, exec, ~/dev/waybar-ytmusic/ytmusic_ctl.py media-toggle
bindl = , XF86AudioNext,  exec, ~/dev/waybar-ytmusic/ytmusic_ctl.py media-next
bindl = , XF86AudioPrev,  exec, ~/dev/waybar-ytmusic/ytmusic_ctl.py media-prev
```

---

## 🎨 5. Temas de Cores

Na aba **`󰏘 Config`** do player, você pode alternar em tempo real entre **8 temas integrados**:

1. **🌊 Catppuccin Ciano** *(Padrão — `#74c7ec`)*
2. **🔴 YouTube Music** *(Vermelho oficial — `#ff4e45`)*
3. **🟣 Catppuccin Roxo** *(Mauve — `#cba6f7`)*
4. **🌃 Tokyo Night** *(Azul Neon — `#7aa2f7`)*
5. **🧛 Dracula Rosa** *(Rosa/Vampiro — `#ff79c6`)*
6. **🟢 Emerald / Spotify** *(Verde Esmeralda — `#1ed760`)*
7. **🟠 Gruvbox Âmbar** *(Dourado/Âmbar — `#fabd2f`)*
8. **❄️ Nord Ártico** *(Azul Gelo — `#88c0d0`)*

O tema selecionado é salvo em `~/.config/waybar-ytmusic/theme.json`. Para usar cores personalizadas, defina `"theme": "custom"` nesse arquivo e edite os valores hexadecimais (`bg_main`, `bg_card`, `bg_btn`, `text_main`, `text_sub`, `accent`, `accent_hover`, `accent_rgba`, `artist_color`).

---

## 🛠️ 6. Comandos Úteis via CLI (`ytmusic_ctl.py`)

```bash
./ytmusic_ctl.py status             # Exibe JSON com o estado atual da reprodução
./ytmusic_ctl.py toggle             # Play / Pause
./ytmusic_ctl.py next               # Próxima música
./ytmusic_ctl.py prev               # Música anterior
./ytmusic_ctl.py vol-up             # Aumenta o volume em +5%
./ytmusic_ctl.py vol-down           # Diminui o volume em -5%
./ytmusic_ctl.py sync-waybar-theme  # Sincroniza o tema atual com o style.css da Waybar
```

---

## 🔄 7. Fluxo de Desenvolvimento & Git

- Repositório remoto: `git@github.com:denilsonoIiveira/waybartube.git` (branch `main`).
- **Regra de versionamento**: A cada alteração realizada no projeto, o assistente/desenvolvedor deve **sempre perguntar antes se pode fazer o commit e subir (`git push`) as alterações para o Git**.

