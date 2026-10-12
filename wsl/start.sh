#!/usr/bin/env bash
# Inicia o desktop Linux local (KasmVNC + Wine/Vinegar + gateway em http://localhost:8080).
# Sai com código 0 somente quando o desktop está pronto para uso.
set -Eeuo pipefail

ROOT="$HOME/roblox-linux"
APP="$ROOT/app"
TOKEN_FILE="$HOME/.config/roblox-linux/token"
STATE="$HOME/.cache/roblox-linux"
URL="http://127.0.0.1:8080"
PID_FILE="$STATE/entrypoint.pid"
LOG_FILE="$STATE/run.log"

[[ -s "$TOKEN_FILE" ]] || { echo "ERRO: token ausente. Execute o provisionamento (RobloxLinux.exe install)." >&2; exit 1; }

health() { curl -fsS -m 2 "$URL/health" 2>/dev/null; }

if health | grep -q '"desktop":"up"'; then
  echo "Desktop já está em execução."
  exit 0
fi

mkdir -p "$STATE"
: > "$LOG_FILE"

export ACCESS_TOKEN
ACCESS_TOKEN="$(tr -d '\r\n' < "$TOKEN_FILE")"
export PORT=8080
export APP_DIR="$APP"
export STATIC_DIR="$APP/docs"
export VINEGAR_TEMPLATE="$ROOT/vinegar-config.toml.template"
export VNC_RESOLUTION="${VNC_RESOLUTION:-1920x1080}"
export ROBLOX_AUTOSTART="${ROBLOX_AUTOSTART:-0}"

echo "Iniciando desktop..."
nohup bash "$APP/entrypoint.sh" >> "$LOG_FILE" 2>&1 &
echo $! > "$PID_FILE"

for _ in $(seq 1 120); do
  if health | grep -q '"desktop":"up"'; then
    echo "READY"
    exit 0
  fi
  if ! kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    echo "ERRO: o desktop encerrou durante a inicialização. Últimas linhas do log:" >&2
    tail -n 60 "$LOG_FILE" >&2
    exit 1
  fi
  sleep 1
done

echo "ERRO: tempo esgotado aguardando o desktop. Últimas linhas do log:" >&2
tail -n 60 "$LOG_FILE" >&2
exit 1
