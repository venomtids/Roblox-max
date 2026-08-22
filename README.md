# Roblox Fixer & Launcher Pro — executável nativo

Aplicativo desktop nativo para Windows 10/11. **Não usa e não instala Python, Node.js, npm ou qualquer runtime externo.**

## Baixar e executar

Baixe este arquivo diretamente:

[`release/Roblox-Fixer-Launcher-Pro.exe`](release/Roblox-Fixer-Launcher-Pro.exe)

Depois, dê dois cliques no `.exe`. O programa usa apenas componentes já incluídos no Windows.

## Recursos

- Visual moderno com modo claro/escuro automático do Windows.
- Limpeza de caches, logs e temporários do Roblox.
- Redefinição do arquivo `GlobalBasicSettings_13.xml`.
- Limpeza do DNS e redefinição do Winsock sem abrir Prompt de Comando visível.
- Launcher direto por ID numérico usando o protocolo `roblox-player:`.
- Interface responsiva: reparos são executados em uma thread separada.
- Binário de interface gráfica (`Windows GUI`), portanto não abre terminal.

A otimização de rede abre somente a confirmação padrão do UAC, pois o Windows exige permissão administrativa para redefinir o Winsock.

## Verificação do arquivo

SHA-256:

```text
f939186eb0677ec637b034a4d84234adb98c2d64a50881fa34d95a2f0f591c73
```

## Código-fonte nativo

O código está em [`native/roblox_fixer_native.c`](native/roblox_fixer_native.c) e utiliza diretamente a API Win32. O executável incluído foi compilado para Windows x64 como subsistema `Windows GUI`.
