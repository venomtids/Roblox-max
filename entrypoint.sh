#!/usr/bin/env bash
# Entrypoint do container Cloud Linux Desktop.
#
# Ordem de inicialização:
#   1. Prepara HOME, XDG_* e diretórios de runtime.
#   2. Gera ~/.vnc/kasmvnc.yaml (WebSocket apenas em loopback).
#   3. Inicia o display virtual KasmVNC (Xvnc + XFCE4) em $DISPLAY.
#   4. Aguarda a porta WebSocket do KasmVNC responder.
#   5. Copia o template do Vinegar (uma única vez) e, opcionalmente, inicia o Roblox.
#   6. Inicia o gateway Node.js na porta pública $PORT (proxy WebSocket + auth).
#   7. Monitora o KasmVNC: se cair, encerra o container para que a plataforma reinicie.
set -Eeuo pipefail

log() { printf '[entrypoint %s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERRO: $*"; exit 1; }

# --- Variáveis com padrões seguros -----------------------------------------
: "${ACCESS_TOKEN:?defina ACCESS_TOKEN (mínimo 16 caracteres)}"
: "${PORT:=8080}"
: "${DISPLAY:=:1}"
: "${VNC_PORT:=6901}"
: "${VNC_RESOLUTION:=1920x1080}"
: "${VNC_DEPTH:=24}"
: "${VNC_DESKTOP:=xfce}"
: "${ROBLOX_AUTOSTART:=0}"
: "${ROBLOX_LAUNCH_CMD:=vinegar}"

export HOME="${HOME:-/home/desktop}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
export XDG_RUNTIME_DIR="/tmp/runtime-$(id -u)"
export LANG="${LANG:-C.UTF-8}"
export DISPLAY
export PULSE_SERVER="${PULSE_SERVER:-}"

display_num="${DISPLAY#:}"
display_num="${display_num%%.*}"

mkdir -p "$HOME/.vnc" "$XDG_CONFIG_HOME/vinegar" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

port_open() {
  timeout 1 bash -c "exec 3<>/dev/tcp/127.0.0.1/$1" 2>/dev/null
}

# --- 2. Configuração do KasmVNC ---------------------------------------------
# O WebSocket do KasmVNC fica somente em 127.0.0.1. O único acesso externo é o gateway Node,
# que valida o token antes de fazer o proxy de /websockify.
cat > "$HOME/.vnc/kasmvnc.yaml" <<EOF
network:
  interface: 127.0.0.1
  websocket_port: ${VNC_PORT}
  ssl:
    require_ssl: false

desktop:
  resolution:
    width: ${VNC_RESOLUTION%x*}
    height: ${VNC_RESOLUTION#*x}
  pixel_depth: ${VNC_DEPTH}
  allow_resize: false

logging:
  log_dest: logfile
  level: 30
EOF

# --- 3. Display virtual + XFCE4 ---------------------------------------------
# Limpa locks de um run anterior (ex.: restart do container sem limpar /tmp).
rm -f "/tmp/.X${display_num}-lock" "/tmp/.X11-unix/X${display_num}" 2>/dev/null || true

log "Iniciando KasmVNC em ${DISPLAY} (${VNC_RESOLUTION}x${VNC_DEPTH}, WebSocket 127.0.0.1:${VNC_PORT}, desktop ${VNC_DESKTOP})"
# -localhost: o socket RFB bruto também só aceita loopback.
# -SecurityTypes None / -disableBasicAuth: a autenticação é feita pelo gateway (ACCESS_TOKEN).
vncserver "${DISPLAY}" \
  -geometry "${VNC_RESOLUTION}" \
  -depth "${VNC_DEPTH}" \
  -select-de "${VNC_DESKTOP}" \
  -websocketPort "${VNC_PORT}" \
  -localhost \
  -SecurityTypes None \
  -disableBasicAuth \
  >"$HOME/.vnc/startup.log" 2>&1 || {
    log "vncserver retornou erro. Últimas linhas:"
    tail -n 60 "$HOME"/.vnc/*.log 2>/dev/null || true
    die "falha ao iniciar KasmVNC"
  }

# --- 4. Aguarda o WebSocket do KasmVNC --------------------------------------
for _ in $(seq 1 60); do
  if port_open "$VNC_PORT"; then
    log "KasmVNC pronto na porta ${VNC_PORT}"
    break
  fi
  sleep 1
done
port_open "$VNC_PORT" || {
  tail -n 80 "$HOME"/.vnc/*.log 2>/dev/null || true
  die "KasmVNC não abriu a porta ${VNC_PORT} em 60s"
}

# --- 5. Vinegar / Roblox -----------------------------------------------------
VINEGAR_CONFIG="$XDG_CONFIG_HOME/vinegar/config.toml"
if [[ ! -f "$VINEGAR_CONFIG" ]]; then
  log "Criando configuração inicial do Vinegar em ${VINEGAR_CONFIG}"
  cp /etc/cloud-desktop/vinegar-config.toml.template "$VINEGAR_CONFIG"
fi

if [[ "$ROBLOX_AUTOSTART" == "1" ]]; then
  log "ROBLOX_AUTOSTART=1 -> executando: ${ROBLOX_LAUNCH_CMD}"
  nohup bash -c "$ROBLOX_LAUNCH_CMD" >"$XDG_CACHE_HOME/roblox-launch.log" 2>&1 &
fi

# --- 6. Gateway Node.js (processo principal) --------------------------------
log "Iniciando gateway na porta ${PORT}"
PORT="$PORT" VNC_PORT="$VNC_PORT" node /opt/cloud-desktop/server.js &
gateway_pid=$!

cleanup() {
  log "Encerrando serviços..."
  kill -TERM "$gateway_pid" 2>/dev/null || true
  wait "$gateway_pid" 2>/dev/null || true
  vncserver -kill "${DISPLAY}" >/dev/null 2>&1 || true
  log "Encerrado."
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

# --- 7. Watchdog do KasmVNC --------------------------------------------------
parent_pid=$$
(
  while sleep 15; do
    if ! port_open "$VNC_PORT"; then
      printf '[watchdog] KasmVNC deixou de responder; solicitando reinício do container\n' >&2
      kill -TERM "$parent_pid" 2>/dev/null || true
      exit 0
    fi
  done
) &

wait "$gateway_pid"
