# Roblox Bootstrapper Advanced

Bootstrapper nativo para Windows 10/11, com interface Win32 moderna em tema escuro fixo. Não usa Python, Node.js, .NET, WebView, WebView2 ou bibliotecas gráficas externas na própria interface.

## Download

[`../release/Roblox-Bootstrapper-Advanced.exe`](../release/Roblox-Bootstrapper-Advanced.exe)

## Recursos

### Inicialização

- Place ID público.
- Links oficiais atuais de servidor privado (`/share?code=...&type=Server`).
- Links privados legados (`privateServerLinkCode`).
- Validação do domínio `roblox.com`.
- Disparo pelo protocolo oficial `roblox:`.
- Reabertura opcional limitada a três tentativas, desativada por padrão.

### Diagnóstico

- Confere se o protocolo `roblox:` está registrado.
- Detecta o WebView2 Runtime nos registros de 32 e 64 bits.
- Confere a presença da pasta de versões do Roblox.
- Informa memória física disponível.
- Detecta se `RobloxPlayerBeta.exe` está em execução.
- Localiza e analisa os últimos 256 KB do log mais recente.
- Identifica indícios de WebView, Direct3D/GPU, falta de memória e crashes.
- Abre o log mais recente no Bloco de Notas para análise manual.

### Reparo e atualização

- Consulta a versão pelo serviço oficial `clientsettingscdn.roblox.com`.
- Baixa o instalador correspondente de `setup.rbxcdn.com`.
- Confirma o cabeçalho de executável Windows.
- Valida a assinatura Authenticode antes de apagar a instalação antiga.
- Interrompe e remove o download quando a assinatura não é confiável.
- Fecha os processos do Roblox apenas após confirmação do usuário.
- Remove arquivos locais corrompidos e executa o instalador oficial.
- Exibe cada etapa na área de status.

### Limpeza

O botão **Limpar Cache** remove apenas downloads temporários, logs, cache HTTP, armazenamento local de cache e `%TEMP%\Roblox`.

## Segurança

- Nunca solicita `.ROBLOSECURITY`, senhas ou cookies.
- Não injeta código, não instala hooks e não lê a memória do jogo.
- Não modifica `RobloxPlayerBeta.exe`.
- O monitor opcional consulta somente a lista de processos.
- O reparo usa exclusivamente servidores oficiais e valida a assinatura do instalador.

O executável deste projeto não tem certificado comercial de assinatura de código. O fonte completo está em [`roblox_bootstrapper_advanced.c`](roblox_bootstrapper_advanced.c).

## Integridade

SHA-256:

```text
a48e9417d2f3114f84682e421884c6b4bab8475e1060c2b73d2024416172b656
```

## Compilação

Exemplo com Zig 0.15.2:

```bash
zig cc roblox_bootstrapper_advanced.c \
  -target x86_64-windows-gnu -O2 -municode \
  -Wl,--subsystem,windows \
  -lshell32 -ladvapi32 -ldwmapi -lgdi32 -luser32 -lkernel32 \
  -lurlmon -lwininet -lwintrust -lcrypt32 \
  -o Roblox-Bootstrapper-Advanced.exe
```
