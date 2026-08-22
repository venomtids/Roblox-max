# Roblox Fixer & Launcher Pro — executável nativo

Aplicativo nativo para Windows 10/11. **Não usa nem instala Python, Node.js, npm ou runtimes externos.**

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
4. Baixa o instalador exclusivamente de `roblox.com`.
5. Valida a assinatura Authenticode do instalador com o Windows.
6. Apaga o download e interrompe o reparo se a assinatura não for confiável.
7. Executa o instalador oficial mais recente.

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
2caf8acc4ec2ee25fcd7b4db63059101c62c44df67dbe94a17aeb8d82eef00f0
```

## Referências oficiais

- [Problemas gerais do Roblox no desktop](https://en.help.roblox.com/hc/en-us/articles/203312870-General-Desktop-Roblox-Issues)
- [Como reinstalar o Roblox](https://en.help.roblox.com/hc/en-us/articles/203312910-How-Do-I-Reinstall-the-Roblox-Software)
- [Problemas gráficos em computadores](https://en.help.roblox.com/hc/en-us/articles/203312790-Graphics-Problems-on-Computers)
- [Mensagens do anti-cheat](https://en.help.roblox.com/hc/en-us/articles/24275616578708-Anti-cheat-Messages)
