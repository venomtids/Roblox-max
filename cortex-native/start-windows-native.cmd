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
  echo Primeiro uso: treinando o GRU auxiliar e o embedding hibrido.
  build\cortex.exe train --corpus data\corpus.pt.txt --state state
  if errorlevel 1 exit /b 1
)

set SELF_EDIT_FLAGS=--enable-self-edit
set SELF_EDIT_LABEL=autoedicao nativa controlada
if /I "%~1"=="--system" (
  set SELF_EDIT_FLAGS=--unrestricted-self-edit
  set SELF_EDIT_LABEL=autoedicao nativa irrestrita supervisionada
)

echo Iniciando Cortex Cognitive Mesh em http://127.0.0.1:8080
echo Modo 100%% nativo: sem GGUF, sem API e sem llama-server.
echo Recursos: sintese, escrita, resumo, codigo, documentos, internet, memoria e %SELF_EDIT_LABEL%.
start "" http://127.0.0.1:8080
build\cortex.exe serve --state state --host 127.0.0.1 --port 8080 --power medium %SELF_EDIT_FLAGS% --source-root .
endlocal
