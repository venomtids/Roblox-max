# Executar Cortex no Windows 10/11

A Cortex continua sendo C++20 nativo. Estes passos não instalam nem executam Python, Node.js, npm, .NET ou WebView.

## Caminho rápido

### 1. Compilador

Instale **MSYS2 com MinGW-w64 x64**, abra o terminal `MSYS2 MinGW x64` e confirme:

```text
g++ --version
```

### 2. Modelo aberto local

No PowerShell ou Prompt de Comando:

```powershell
winget install --exact --id ggml.llamacpp
```

Isso fornece `llama-server.exe`, o runtime C++ que executa pesos GGUF. Nenhuma chave é necessária.

### 3. Compilar, testar e iniciar

Na pasta `cortex-native`, execute:

```bat
build-windows.cmd
start-windows.cmd
```

Para o modo que pode criar/substituir qualquer arquivo e executar validações após autorização, use:

```bat
start-windows-system.cmd
```

O próprio `cortex.exe` usa a API nativa `ShellExecuteExW` para exibir o prompt UAC antes de conceder privilégios administrativos à sessão; não há PowerShell, Python ou Node nesse fluxo.

`build-windows.cmd` compila os testes e só gera `build\cortex.exe` quando eles passam. `start-windows.cmd`:

1. treina o GRU auxiliar de aproximadamente 1 milhão de parâmetros se `state` ainda não existir;
2. inicia o perfil **Qwen3 1.7B Q8**, portanto supera um bilhão de parâmetros funcionais;
3. baixa o GGUF oficial no primeiro uso por meio do próprio llama.cpp;
4. inicia o chat em `http://127.0.0.1:8080`;
5. habilita a autoevolução controlada somente no loopback. `start-windows-system.cmd` solicita UAC e troca para o escopo irrestrito supervisionado.

O download inicial do Qwen3 1.7B é de aproximadamente 1,83 GB. Depois de armazenado no cache do llama.cpp, o modelo pode funcionar offline. Recomenda-se Windows x64 com pelo menos 4 GiB de RAM; 8 GiB ou mais oferece margem melhor para contexto e outros programas.

## Validação contínua opcional

`ci\cortex-windows.yml.example` contém o job Windows 2022/MinGW-w64. Para ativá-lo em uma conexão GitHub com permissão de workflows, copie-o para `.github\workflows\cortex-windows.yml`.

## Diagnóstico

```bat
build\cortex.exe doctor --state state
build\cortex.exe models
```

O diagnóstico verifica estado GRU, RAM, `curl.exe`, `llama-server.exe` e prontidão para a camada de 1,7B.

## Modos de potência

```bat
build\cortex.exe serve --state state --billion --power flash
build\cortex.exe serve --state state --billion --power medium
build\cortex.exe serve --state state --billion --power high
```

- **Flash:** menor contexto e até 256 tokens; prioriza latência.
- **Medium:** até 1.024 tokens, seis memórias e equilíbrio geral.
- **High:** até 4.096 tokens, doze memórias e uma segunda passagem de revisão quando a janela do modelo permite.

O contexto real também é limitado pelo perfil GGUF carregado. Mais potência significa maior uso de RAM e mais latência.

## Autoevolução

Modo limitado ao projeto:

```bat
build\cortex.exe serve --state state --host 127.0.0.1 --billion --power high --enable-self-edit --source-root .
```

Modo irrestrito supervisionado:

```bat
start-windows-system.cmd
```

Esse segundo script pede autorização UAC e inicia `--unrestricted-self-edit`. Na interface, a Cortex pode:

1. auditar qualquer arquivo regular que a conta consiga ler;
2. propor a substituição completa ou criar um caminho novo;
3. mostrar destino e conteúdo antes da escrita;
4. aplicar somente após `APLICAR N`;
5. preparar qualquer comando de validação e executá-lo somente após `EXECUTAR N`;
6. restaurar arquivo anterior ou remover arquivo criado com `REVERTER N`, inclusive após reiniciar.

Não há allowlist de diretório, extensão ou conteúdo nesse modo. Permanecem a permissão explícita por operação, hash contra conflito e backup para impedir ações silenciosas. Se o usuário negar o UAC ou a conta não possuir acesso, o Windows bloqueia a alteração.

## Modelo GGUF já baixado

Também é possível apontar diretamente para um arquivo:

```bat
build\cortex.exe serve --state state --local-model C:\modelos\modelo.gguf --local-name MeuModelo --power high
```

Use pesos cuja licença permita seu caso de uso e mantenha modelos grandes fora do Git.
