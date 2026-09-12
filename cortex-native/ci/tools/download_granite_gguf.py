#!/usr/bin/env python3
"""Baixa o GGUF oficial do Granite 4.2 para o teste de integração real.

Descobre o nome exato do arquivo pela API pública do Hugging Face em vez de
fixar o nome no workflow: quantizações do mesmo modelo aparecem com sufixos
diferentes entre revisões, e um nome fixo quebraria o agendamento sem aviso.

Só usa a biblioteca padrão. O token é opcional (HF_TOKEN) porque o Granite é
Apache-2.0 e não exige autenticação.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import urllib.request

USER_AGENT = "cortex-native-ci/1.0"


def fetch_json(url: str) -> object:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    token = os.environ.get("HF_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read().decode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Baixa o GGUF do Granite")
    parser.add_argument("--repository", required=True)
    parser.add_argument("--quantization", required=True)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--minimum-megabytes", type=int, default=100)
    arguments = parser.parse_args()

    entries = fetch_json(f"https://huggingface.co/api/models/{arguments.repository}/tree/main")
    if not isinstance(entries, list):
        raise SystemExit(f"resposta inesperada da API do Hugging Face: {entries!r}")

    wanted = arguments.quantization.lower()
    candidates: list[tuple[str, int]] = []
    for entry in entries:
        path = str(entry.get("path", ""))
        size = int(entry.get("size", 0) or 0)
        if path.lower().endswith(".gguf") and wanted in path.lower():
            candidates.append((path, size))
    if not candidates:
        available = ", ".join(str(entry.get("path")) for entry in entries)[:600]
        raise SystemExit(f"nenhum GGUF com '{arguments.quantization}' em {arguments.repository}; "
                         f"arquivos: {available}")

    # Se houver divisão em partes, escolhe o maior arquivo (a parte principal).
    path, size = max(candidates, key=lambda item: item[1])
    if size < arguments.minimum_megabytes * 1024 * 1024:
        raise SystemExit(f"arquivo suspeito: {path} tem apenas {size} bytes")

    arguments.output.mkdir(parents=True, exist_ok=True)
    destination = arguments.output / pathlib.Path(path).name
    url = f"https://huggingface.co/{arguments.repository}/resolve/main/{path}"
    print(f"baixando {url}\n  -> {destination} ({size / (1024 * 1024):.0f} MiB)", flush=True)

    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    token = os.environ.get("HF_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=300) as response, \
            destination.open("wb") as target:
        downloaded = 0
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            target.write(chunk)
            downloaded += len(chunk)
            if downloaded % (64 * 1024 * 1024) < 1024 * 1024:
                print(f"  {downloaded / (1024 * 1024):.0f} MiB", flush=True)

    if downloaded < arguments.minimum_megabytes * 1024 * 1024:
        raise SystemExit(f"download incompleto: {downloaded} bytes")
    print(f"ok: {destination} ({downloaded} bytes)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
