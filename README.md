# Roblox Fixer & Launcher Pro

Aplicativo desktop para Windows, feito com Python e CustomTkinter, com tema claro/escuro automático. Ele oferece limpeza segura dos caches do Roblox, redefinição do XML gráfico, limpeza de DNS/redefinição de Winsock e abertura direta por ID.

## Executar o código

Requer Python 3.10 ou mais recente no Windows.

```powershell
py -m pip install -r requirements.txt
py roblox_fixer_launcher.py
```

> A redefinição do Winsock exige privilégios elevados. Ao executar o script sem elevação, abra o PowerShell como administrador. O executável recomendado abaixo solicita a permissão automaticamente.

## Gerar o `.exe` sem ter Python ou Node.js instalado

No Windows, dê dois cliques em:

```text
GERAR_EXECUTAVEL.bat
```

O gerador baixa o Python oficial e as dependências automaticamente, compila o programa e abre a pasta do executável ao terminar. É necessária uma conexão com a internet somente durante a geração.

### Comando manual alternativo

Se o Python já estiver instalado, execute no PowerShell:

```powershell
py -m PyInstaller --noconsole --onefile --uac-admin --clean --name "Roblox Fixer Launcher Pro" --collect-all customtkinter roblox_fixer_launcher.py
```

O resultado será criado em:

```text
dist\Roblox Fixer Launcher Pro.exe
```

- `--noconsole`: usa o modo *windowed* e impede a abertura do Prompt de Comando.
- `--onefile`: empacota aplicação e dependências em um único `.exe`.
- `--uac-admin`: solicita a permissão necessária para `netsh winsock reset`.
- `--collect-all customtkinter`: inclui temas e recursos visuais do CustomTkinter.

O computador de destino não precisa ter Python instalado. O executável deve ser compilado no Windows; um `.exe` não é gerado corretamente por uma compilação comum em Linux/macOS.

## Segurança e comportamento

- O ID do mapa aceita somente dígitos antes de ser enviado ao protocolo do Roblox.
- Os comandos do sistema são executados em segundo plano com `CREATE_NO_WINDOW`.
- A limpeza é limitada às pastas de cache, logs e temporários do Roblox; instalações e dados externos não são removidos.
- O Roblox Player precisa estar instalado e associado ao protocolo `roblox-player:`.
