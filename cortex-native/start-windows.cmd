@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

if not exist build\cortex.exe (
  echo Cortex ainda nao foi compilada. Executando build-windows.cmd...
  call build-windows.cmd
  if errorlevel 1 exit /b 1
)

if not exist state\model.cgru (
  echo Primeiro uso: treinando o GRU auxiliar de aproximadamente 1 milhao de parametros.
  build\cortex.exe train --corpus data\corpus.pt.txt --state state
  if errorlevel 1 exit /b 1
)

where llama-server.exe >nul 2>nul
if errorlevel 1 (
  if "%CORTEX_LLAMA_SERVER%"=="" (
    echo ERRO: llama-server.exe nao foi encontrado.
    echo Instale o runtime C++ oficial com:
    echo   winget install --exact --id ggml.llamacpp
    echo Depois feche e abra o terminal e execute este arquivo novamente.
    exit /b 1
  )
)

set SELF_EDIT_FLAGS=--enable-self-edit
set SELF_EDIT_LABEL=autoevolucao controlada
if /I "%~1"=="--system" (
  set SELF_EDIT_FLAGS=--unrestricted-self-edit
  set SELF_EDIT_LABEL=autoevolucao irrestrita supervisionada
)

echo Iniciando Cortex em http://127.0.0.1:8080
echo Perfil: Qwen3 1.7B local, modo Medium, pesquisa e %SELF_EDIT_LABEL%.
start "" http://127.0.0.1:8080
build\cortex.exe serve --state state --host 127.0.0.1 --port 8080 --billion --power medium %SELF_EDIT_FLAGS% --source-root .
endlocal
