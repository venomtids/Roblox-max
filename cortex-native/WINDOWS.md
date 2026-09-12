# Cortex no Windows 10/11

Cortex é compilada como um executável C++20 nativo. O fluxo abaixo não instala Python, Node.js, npm, .NET, Docker ou WebView. A interface abre no navegador padrão, mas é servida pelo próprio `cortex.exe`.

## Requisitos do perfil Samsung X40

- Windows 10/11 x64;
- aproximadamente 6 GiB de RAM total ou mais para o perfil `x40` (8 GiB oferece margem mais segura);
- cerca de 2,24 GB para o GGUF, além do estado e cache;
- espaço temporário durante download/compilação;
- internet no primeiro download; depois o modelo pode funcionar offline.

O perfil X40 executa **IBM Granite 4.2 3B Q4_K_M**, contexto 4.096, quatro threads e CPU-only. Em uma máquina com apenas 4 GiB, prefira `--local-profile edge`, que usa Q3_K_M e contexto menor. O HD torna download e inicialização mais lentos, mas não impede o funcionamento.

## Opção 1 — instalação assistida

Na pasta `cortex-native`, dê dois cliques em:

```text
Cortex-One-Click-Windows.cmd
```

O script:

1. verifica `winget`;
2. instala MSYS2/MinGW-w64 quando necessário;
3. instala o runtime C++ `llama.cpp`;
4. compila e executa as 130 verificações;
5. abre Cortex no modo X40;
6. solicita UAC para o escopo irrestrito supervisionado.

UAC concede apenas privilégios ao processo. Mesmo elevado, cada escrita ainda exige `APLICAR N`, cada comando exige `EXECUTAR N` e cada restauração exige `REVERTER N`.

Se `llama-server.exe` ainda não aparecer no `PATH` após a instalação, feche a janela e execute o arquivo novamente.

## Opção 2 — instalação manual

### 1. Instalar MinGW-w64

Instale MSYS2 e, no terminal MSYS2, execute:

```bash
pacman -Sy --noconfirm --needed mingw-w64-x86_64-gcc make
```

Abra o terminal **MSYS2 MinGW x64** e confirme:

```text
g++ --version
```

### 2. Instalar llama.cpp

No PowerShell ou Prompt de Comando:

```powershell
winget install --exact --id ggml.llamacpp
```

Confirme em um novo terminal:

```bat
where llama-server.exe
```

Se o executável estiver em outro diretório, configure apenas nesta sessão:

```bat
set CORTEX_LLAMA_SERVER=C:\caminho\para\llama-server.exe
```

### 3. Compilar e testar

Na pasta `cortex-native`:

```bat
build-windows.cmd
```

O script compila `build\cortex-tests.exe`, exige que todos os testes passem e só então cria `build\cortex.exe`. As bibliotecas vinculadas além da runtime C++ são componentes nativos do Windows: `ws2_32` e `shell32`.

### 4. Iniciar

Perfil X40 recomendado:

```bat
start-windows.cmd
```

Modo totalmente nativo, sem baixar GGUF:

```bat
start-windows-native.cmd
```

Modo irrestrito supervisionado, com UAC:

```bat
start-windows-system.cmd
```

Depois, acesse:

```text
http://127.0.0.1:8080
```

No primeiro uso, os scripts também treinam o GRU auxiliar quando `state\model.cgru` ainda não existe.

## Comandos diretos

Diagnóstico:

```bat
build\cortex.exe doctor --state state
build\cortex.exe models
```

Chat X40:

```bat
build\cortex.exe serve --state state --host 127.0.0.1 --port 8080 --x40 --power medium
```

O antigo argumento `--billion` permanece apenas como alias de `--x40`; o perfil carregado continua sendo Granite.

Seleção automática conforme RAM:

```bat
build\cortex.exe serve --state state --local --power medium
```

Forçar o perfil mais leve:

```bat
build\cortex.exe serve --state state --local-profile edge --power flash
```

## Modos de potência

```bat
build\cortex.exe serve --state state --x40 --power flash
build\cortex.exe serve --state state --x40 --power medium
build\cortex.exe serve --state state --x40 --power high
```

| Modo | Comportamento Granite | Custo esperado |
|---|---|---|
| Flash | thinking desligado, resposta curta | menor latência/RAM de contexto |
| Medium | low effort, até 1.024 tokens | equilíbrio recomendado |
| High | thinking completo e revisão | maior latência e contexto |

Em High, o limite lógico é 4.096 tokens, mas entrada, raciocínio e saída compartilham a janela real do modelo.

## Ensinar documentos locais

Um arquivo:

```bat
build\cortex.exe ingest --file C:\documentos\manual.md --source manual --state state
```

Uma pasta inteira:

```bat
build\cortex.exe ingest --directory C:\documentos --source biblioteca --max-files 512 --state state
```

Consultar:

```bat
build\cortex.exe recall --query "como funciona o procedimento?" --state state --limit 5
```

A varredura ignora `.git`, `build`, `state`, `node_modules`, `target` e diretórios ocultos. Arquivos binários, vazios, maiores que 8 MiB ou além do limite total são ignorados. Use lotes menores no HD para facilitar auditoria.

## Autoevolução

Limitada ao projeto:

```bat
build\cortex.exe serve --state state --host 127.0.0.1 --x40 --power high --enable-self-edit --source-root .
```

Irrestrita e elevada:

```bat
start-windows-system.cmd
```

O painel permite:

1. sugerir automaticamente uma fonte usando auditoria e telemetria agregada persistente;
2. auditar um arquivo escolhido;
3. gerar uma proposta segmentada por SEARCH/REPLACE;
4. ver o diff antes da escrita;
5. aplicar somente com `APLICAR N`;
6. registrar uma validação sem executá-la;
7. executar somente com `EXECUTAR N`;
8. restaurar com `REVERTER N`, inclusive após reiniciar.

Arquivos grandes não são enviados inteiros ao modelo: Cortex seleciona trechos relevantes, exige busca exata e única e recompõe o arquivo em memória. Backup, hash e escrita atômica evitam sobrescrever uma alteração concorrente.

No modo irrestrito não existe allowlist fixa de extensão ou diretório. Continuam obrigatórios loopback, confirmação por operação, limites de tamanho, validação básica, journal e rollback. Se o usuário negar UAC ou a conta não tiver permissão NTFS, o Windows bloqueia a operação.

## Usar um GGUF já baixado

```bat
build\cortex.exe serve --state state ^
  --local-model C:\modelos\granite.gguf ^
  --local-name "Granite local" ^
  --local-context 4096 ^
  --local-threads 4 ^
  --local-gpu-layers 0 ^
  --power medium
```

Mantenha pesos fora do Git e confirme a licença do modelo para seu caso de uso.

## Solução de problemas

### `g++` não encontrado

Use o terminal **MSYS2 MinGW x64** ou inclua `C:\msys64\mingw64\bin` no `PATH` da sessão.

### `llama-server.exe` não encontrado

Abra um terminal novo após `winget`, rode `where llama-server.exe` ou defina `CORTEX_LLAMA_SERVER`.

### Falta de memória ou paginação intensa

Feche navegador/jogos, use `--local-profile edge`, reduza para Flash e mantenha contexto menor. Se o LED do HD permanecer ativo e a resposta congelar, a máquina provavelmente está paginando.

### Primeiro download demora

O GGUF possui aproximadamente 2,24 GB. Não interrompa a janela; o cache do llama.cpp permite uso offline nas próximas execuções.

### A porta 8080 já está ocupada

```bat
build\cortex.exe serve --state state --host 127.0.0.1 --port 8090 --x40
```

Abra `http://127.0.0.1:8090`.

### Verificação final

Antes de usar autoedição em arquivos importantes:

```bat
build\cortex-tests.exe
build\cortex.exe doctor --state state
build\cortex.exe benchmark --state state --steps 1000
```

Mantenha o projeto sob Git e teste primeiro em uma cópia ou branch. Nenhuma métrica interna substitui a revisão humana do diff e do resultado da compilação.
