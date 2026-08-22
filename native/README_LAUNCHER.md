# Roblox Native Launcher — Win32 C

Versão pronta para Windows 10/11, compilada diretamente para `.exe`.

## Download

[`../release/Roblox-Native-Launcher.exe`](../release/Roblox-Native-Launcher.exe)

Não requer Python, Node.js, .NET, WebView, WebView2, navegador ou instalação de runtime adicional.

## Comportamento

- Aceita Place ID numérico.
- Aceita links privados oficiais atuais (`/share?code=...&type=Server`).
- Aceita links privados legados (`privateServerLinkCode`).
- Recusa domínios que não sejam `roblox.com`.
- Nunca pede nem processa `.ROBLOSECURITY`.
- Usa `ShellExecuteW` para entregar o protocolo `roblox:` ao Windows.
- Não abre processo, não injeta código, não lê memória e não instala hooks.
- Não mantém comunicação com o Roblox depois do disparo.

## Compilação reproduzível

O fonte está em `roblox_launcher_native.c`. Exemplo com Zig 0.15.2:

```bash
zig cc roblox_launcher_native.c \
  -target x86_64-windows-gnu \
  -O2 -municode \
  -Wl,--subsystem,windows \
  -lshell32 -ladvapi32 -ldwmapi -lgdi32 -luser32 -lkernel32 \
  -o Roblox-Native-Launcher.exe
```

O subsistema é `Windows GUI`, portanto nenhuma janela de terminal é aberta.

## Integridade

SHA-256:

```text
69434e1815051cbc9d0c624ad018fc97305a175b110c7682f53a6723be96fdde
```

O binário não possui certificado comercial de assinatura de código. O hash e o fonte permitem verificar exatamente o arquivo disponibilizado.
