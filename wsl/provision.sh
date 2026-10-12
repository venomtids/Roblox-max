#!/usr/bin/env bash
# Provisionamento único dentro da distro Debian do WSL2.
#
# Instala: XFCE4, KasmVNC, Mesa, Wine (64/32 bits), Vinegar (compilado a partir do
# upstream), Node.js e o gateway. Copia o app de $RL_WINDIR/app para ~/roblox-linux/app
# e gera o token de acesso local.
#
# Executado pelo RobloxLinux.exe (comando "install"). Pode ser executado de novo com
# segurança: as etapas já concluídas são puladas.
set -Eeuo pipefail

KASMVNC_VERSION="${KASMVNC_VERSION:-1.5.0}"
VINEGAR_VERSION="${VINEGAR_VERSION:-v1.9.4}"
GO_VERSION="${GO_VERSION:-1.26.0}"

ROOT="$HOME/roblox-linux"
APP="$ROOT/app"
TOKEN_DIR="$HOME/.config/roblox-linux"
TOKEN_FILE="$TOKEN_DIR/token"

log()  { printf '\n==> %s\n' "$*"; }
die()  { printf '\nERRO: %s\n' "$*" >&2; exit 1; }

[[ -n "${RL_WINDIR:-}" ]] || die "RL_WINDIR não definido. Execute pelo RobloxLinux.exe install."
[[ -d "$RL_WINDIR/app" ]] || die "Pasta do app não encontrada em $RL_WINDIR/app"

ARCH="$(dpkg --print-architecture)"
case "$ARCH" in
  amd64) GO_ARCH="amd64" ;;
  arm64) GO_ARCH="arm64" ;;
  *) die "Arquitetura não suportada: $ARCH" ;;
esac

if ! grep -q 'ID=debian' /etc/os-release; then
  die "Esta distro não é Debian. Use: wsl --install -d Debian"
fi

# --- 1. Pacotes do sistema --------------------------------------------------
log "Instalando pacotes do sistema (XFCE, Mesa, Wine, Node.js)"
sudo dpkg --add-architecture i386
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  ca-certificates curl \
  xfce4 xfce4-terminal dbus-x11 xauth xfonts-base xdg-utils \
  fonts-noto-core fonts-dejavu-core \
  libgl1-mesa-dri libglx-mesa0 libegl-mesa0 mesa-utils \
  libvulkan1 mesa-vulkan-drivers vulkan-tools \
  wine64 wine32 winetricks cabextract \
  libgtk-4-1 libadwaita-1-0 libglib2.0-bin \
  nodejs npm

# --- 2. KasmVNC (pacote oficial bookworm) -----------------------------------
if ! command -v vncserver >/dev/null 2>&1; then
  log "Instalando KasmVNC ${KASMVNC_VERSION}"
  curl -fsSL -o /tmp/kasmvncserver.deb \
    "https://github.com/kasmtech/KasmVNC/releases/download/v${KASMVNC_VERSION}/kasmvncserver_bookworm_${KASMVNC_VERSION}_${ARCH}.deb"
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /tmp/kasmvncserver.deb
  rm -f /tmp/kasmvncserver.deb
fi

# --- 3. Vinegar (compilado do upstream; go.mod exige Go 1.26) ---------------
if ! command -v vinegar >/dev/null 2>&1; then
  log "Compilando Vinegar ${VINEGAR_VERSION}"
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    make pkg-config g++ libglib2.0-dev-bin gettext libvulkan-dev

  GO_DIR="$HOME/.local/go-${GO_VERSION}"
  if [[ ! -x "$GO_DIR/go/bin/go" ]]; then
    mkdir -p "$GO_DIR"
    curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz" | tar -xz -C "$GO_DIR"
  fi
  export PATH="$GO_DIR/go/bin:$PATH"

  SRC="$HOME/.cache/vinegar-src"
  rm -rf "$SRC" && mkdir -p "$SRC"
  curl -fsSL "https://github.com/vinegarhq/vinegar/archive/refs/tags/${VINEGAR_VERSION}.tar.gz" \
    | tar -xz -C "$SRC" --strip-components=1
  (cd "$SRC" && make all && sudo make install PREFIX=/usr/local)
fi

# --- 4. Copia do app e dependências Node ------------------------------------
log "Copiando o app para ${APP}"
mkdir -p "$ROOT"
rm -rf "$APP"
cp -r "$RL_WINDIR/app" "$APP"
chmod 0755 "$APP/entrypoint.sh" "$APP/wsl/"*.sh

log "Instalando dependências Node (npm ci)"
(cd "$APP" && npm ci --omit=dev --no-audit --no-fund)

cp "$APP/config/vinegar/config.toml.template" "$ROOT/vinegar-config.toml.template"

# --- 5. Token de acesso local ----------------------------------------------
mkdir -p "$TOKEN_DIR"
if [[ ! -s "$TOKEN_FILE" ]]; then
  log "Gerando token de acesso local"
  od -An -tx1 -N24 /dev/urandom | tr -d ' \n' > "$TOKEN_FILE"
  echo >> "$TOKEN_FILE"
  chmod 600 "$TOKEN_FILE"
fi

log "Provisionamento concluído. Use RobloxLinux.exe start para abrir o desktop."
