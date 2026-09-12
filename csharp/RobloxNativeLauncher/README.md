# Roblox Native Launcher — C# / .NET 8 / WinForms

Launcher leve, 100% nativo, sem WebView, WebView2, CefSharp, Python, Node.js ou pacotes de terceiros.

## Segurança

Este projeto **não aceita `.ROBLOSECURITY`**. Esse valor é um cookie de sessão equivalente a uma credencial da conta. Colocá-lo em URI, logs, argumentos ou aplicativos de terceiros pode permitir roubo da conta. O launcher usa somente deeplinks oficiais e públicos do Roblox.

Também não abre, injeta, lê, escreve ou monitora a memória de `RobloxPlayerBeta.exe`. A chamada é *fire-and-forget* por `Process.Start` com `UseShellExecute = true`; o Windows entrega o URI ao protocolo registrado pelo Roblox.

Formatos aceitos:

```text
Place ID público:
roblox://placeId=1818

Link privado atual:
https://www.roblox.com/share?code=CODIGO&type=Server

Link privado legado:
https://www.roblox.com/games/PLACE_ID/nome?privateServerLinkCode=CODIGO
```

O launcher valida o domínio, extrai apenas os parâmetros esperados e reconstrói o deeplink. Links de domínios não oficiais são recusados.

## Estrutura

- `Form1.cs`: janela, validação e disparo.
- `Program.cs`: ponto de entrada WinForms.
- `RobloxNativeLauncher.csproj`: configuração .NET 8 e publicação independente.
- `app.manifest`: execução sem privilégios administrativos e DPI alto.
- `COMPILAR.cmd`: compilação com dois cliques após instalar o SDK.

## Compilar sem Visual Studio

Você não precisa instalar Visual Studio, Python ou Node.js. Instale somente o **.NET 8 SDK** uma vez:

```bat
winget install Microsoft.DotNet.SDK.8
```

Feche e abra o Prompt de Comando. Entre nesta pasta e execute:

```bat
dotnet publish RobloxNativeLauncher.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

Ou dê dois cliques em `COMPILAR.cmd`.

O resultado será:

```text
bin\Release\net8.0-windows\win-x64\publish\RobloxNativeLauncher.exe
```

O EXE publicado é autocontido: o computador em que ele será executado não precisa ter .NET, Python, Node.js ou Visual Studio. O SDK só é necessário no computador que realiza a compilação.

## Outras arquiteturas

Windows ARM64:

```bat
dotnet publish -c Release -r win-arm64 --self-contained true -p:PublishSingleFile=true
```

Para x86, substitua `win-x64` por `win-x86` no comando e no `<RuntimeIdentifier>` do projeto.

## Observação sobre o Roblox

O protocolo `roblox:` deve estar registrado. Se estiver ausente, repare ou reinstale o Roblox Player oficial. O launcher não contorna autenticação, permissões de servidores privados, controles parentais ou regras da experiência.
