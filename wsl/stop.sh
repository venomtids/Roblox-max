#!/usr/bin/env bash
# Encerra o desktop local: envia SIGTERM ao entrypoint, que para o gateway e o KasmVNC.
set -Eeuo pipefail

PID_FILE="$HOME/.cache/roblox-linux/entrypoint.pid"

if [[ ! -s "$PID_FILE" ]]; then
  echo "Desktop não está em execução."
  exit 0
fi

pid="$(cat "$PID_FILE")"
if kill -0 "$pid" 2>/dev/null; then
  kill -TERM "$pid"
  for _ in $(seq 1 20); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
  done
fi
rm -f "$PID_FILE"
echo "Desktop encerrado."
