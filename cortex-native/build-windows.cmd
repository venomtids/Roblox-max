@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

where g++ >nul 2>nul
if errorlevel 1 (
  echo ERRO: g++ MinGW-w64 nao foi encontrado no PATH.
  echo Instale MSYS2/MinGW-w64 e abra o terminal MinGW x64.
  exit /b 1
)

if not exist build mkdir build
set SOURCES=src\core.cpp src\tokenizer.cpp src\neural_model.cpp src\memory.cpp src\quality.cpp src\knowledge.cpp src\agent.cpp src\connectivity.cpp src\local_model.cpp src\reasoner.cpp src\self_editor.cpp src\web_server.cpp src\web_ui.cpp
set FLAGS=-Iinclude -std=c++20 -pthread -Wall -Wextra -Wpedantic -Wconversion -Wshadow

echo [1/3] Compilando testes C++20 para Windows...
g++ %FLAGS% -O2 %SOURCES% tests\test_main.cpp -o build\cortex-tests.exe -lws2_32
if errorlevel 1 exit /b 1

echo [2/3] Executando testes nativos...
build\cortex-tests.exe
if errorlevel 1 exit /b 1

echo [3/3] Compilando Cortex release...
g++ %FLAGS% -O3 -DNDEBUG -flto -static-libgcc -static-libstdc++ %SOURCES% src\main.cpp -o build\cortex.exe -lws2_32
if errorlevel 1 exit /b 1

echo.
echo Build concluido: build\cortex.exe
echo Execute build\cortex.exe doctor --state state
echo Ou use start-windows.cmd para treinar o GRU e iniciar Qwen3 1.7B.
endlocal
