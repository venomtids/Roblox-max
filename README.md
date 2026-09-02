# Roblox Fixer & Launcher Pro — executáveis nativos

Aplicativos nativos para Windows 10/11. **Não usam nem instalam Python, Node.js, npm, .NET, WebView ou runtimes externos.**

## Projeto separado: Cortex Native AI

O agente C++20 agora combina o núcleo próprio de 1 milhão de parâmetros com um reasoner local verificável, calculadora, benchmark real, filtro contra respostas aleatórias, memória persistente, pesquisa HTTPS sem chave (DuckDuckGo com fallback Wikipedia) e modelos abertos Qwen3 de 0,6B a 30,5B via llama.cpp. O comando `pesquise na internet por <assunto>` registra a fonte e guarda o resultado para reutilização offline. Nenhuma API ou chave é necessária; provedores externos permanecem apenas como compatibilidade opt-in. Tudo está isolado em [`cortex-native/`](cortex-native/README.md), sem depender dos launchers Roblox, Python ou Node.js.

## Bootstrapper Advanced — recomendado

Versão completa com layout moderno em tema escuro, launcher, diagnóstico de crashes, análise de logs, limpeza e reparo oficial:

[`release/Roblox-Bootstrapper-Advanced.exe`](release/Roblox-Bootstrapper-Advanced.exe)

Documentação e verificação: [`native/README_BOOTSTRAPPER_ADVANCED.md`](native/README_BOOTSTRAPPER_ADVANCED.md).

## Launcher independente

Baixe o launcher mínimo, sem as ferramentas de reparo:

[`release/Roblox-Native-Launcher.exe`](release/Roblox-Native-Launcher.exe)

Código-fonte: [`native/roblox_launcher_native.c`](native/roblox_launcher_native.c).

O launcher aceita Place ID e links oficiais de servidores privados, valida as entradas e entrega o protocolo `roblox:` ao Windows em modo *fire-and-forget*. Opcionalmente, pode verificar apenas a presença do processo e solicitar até três reaberturas caso ele feche. A opção vem desativada, pode ser desmarcada a qualquer momento e nunca lê a memória do Roblox.

## Novo launcher em C# / .NET 8

O código-fonte completo do launcher WinForms sem WebView está em:

[`csharp/RobloxNativeLauncher`](csharp/RobloxNativeLauncher/README.md)

Ele suporta Place ID e links oficiais de servidores privados, publica como um único EXE autocontido e não acessa o processo ou a memória do Roblox.

## Download

Baixe diretamente:

[`release/Roblox-Fixer-Launcher-Pro.exe`](release/Roblox-Fixer-Launcher-Pro.exe)

Depois, dê dois cliques no `.exe`. O binário é Windows x64 e usa somente componentes já presentes no sistema.

## Correções disponíveis

### Abrir jogos sem Microsoft Edge

O Launcher Direto usa o deeplink nativo:

```text
roblox://experiences/start?placeId=ID
```

Esse protocolo entrega o ID diretamente ao Roblox Player e não abre Edge, Chrome ou outro navegador. O Player oficial precisa estar instalado e conectado à conta.

### Tela branca ou “não está respondendo”

O botão **Reparar Tela Branca / Atualizar Roblox**:

1. Pede confirmação antes de apagar qualquer dado local.
2. Fecha apenas processos do Roblox Player.
3. Remove arquivos locais corrompidos de `%LOCALAPPDATA%\Roblox` e `%TEMP%\Roblox`.
4. Consulta a versão atual no serviço oficial `clientsettingscdn.roblox.com`.
5. Baixa o executável correspondente diretamente de `setup.rbxcdn.com`.
6. Confirma que o download é um executável PE e valida sua assinatura Authenticode.
7. Apaga o download e interrompe o reparo se a assinatura não for confiável.
8. Somente depois da validação remove a instalação corrompida e executa o instalador oficial.
9. Mostra na interface cada etapa: consulta, download, assinatura, limpeza e instalação.

A conta, os Robux, itens e o progresso dos jogos ficam nos servidores e não são apagados. Pode ser necessário entrar novamente na conta.

### Atualização travada

O reparo completo remove versões locais incompletas e executa novamente o bootstrapper oficial. Isso segue a recomendação do suporte do Roblox de reinstalar e limpar os dados locais quando a instalação está corrompida ou desatualizada.

### Outras opções

- Limpeza de caches, logs e temporários.
- Redefinição do `GlobalBasicSettings_13.xml`.
- Limpeza do DNS e redefinição do Winsock.
- Mensagens específicas para falha de internet, assinatura inválida, UAC cancelado ou falha do instalador.

A otimização de rede mostra somente a confirmação padrão do UAC, pois o Windows exige permissão administrativa para redefinir o Winsock.

## Segurança

O programa **não modifica, redesenha, injeta código ou substitui o executável oficial do Roblox**. Modificar o Player poderia acionar o anti-cheat e criar risco de segurança. O reparo instala uma cópia oficial e não usa launchers, extensões ou sites de terceiros.

O executável deste projeto não possui certificado comercial de assinatura de código. O código-fonte está disponível em [`native/roblox_fixer_native.c`](native/roblox_fixer_native.c), e o hash permite conferir se o download não mudou.

SHA-256:

```text
0972f2f206c17f7b26618e2e63cf5cec4a66b386b1c30f089000758bc65c7e87
```

## Referências oficiais

- [Problemas gerais do Roblox no desktop](https://en.help.roblox.com/hc/en-us/articles/203312870-General-Desktop-Roblox-Issues)
- [Como reinstalar o Roblox](https://en.help.roblox.com/hc/en-us/articles/203312910-How-Do-I-Reinstall-the-Roblox-Software)
- [Problemas gráficos em computadores](https://en.help.roblox.com/hc/en-us/articles/203312790-Graphics-Problems-on-Computers)
- [Mensagens do anti-cheat](https://en.help.roblox.com/hc/en-us/articles/24275616578708-Anti-cheat-Messages)
