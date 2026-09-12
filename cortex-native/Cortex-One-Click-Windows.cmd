@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"
title Cortex Native AI - Instalacao em um clique

echo ================================================
echo   Cortex Native AI - instalar, compilar e abrir
echo ================================================
echo.

where winget.exe >nul 2>nul
if errorlevel 1 (
  echo ERRO: winget nao foi encontrado. Atualize o App Installer pela Microsoft Store.
  pause
  exit /b 1
)

if not exist "C:\msys64\usr\bin\bash.exe" (
  echo [1/5] Instalando MSYS2 e o ambiente C++ nativo...
  winget install --exact --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements
  if errorlevel 1 (
    echo ERRO: nao foi possivel instalar MSYS2.
    pause
    exit /b 1
  )
) else (
  echo [1/5] MSYS2 ja esta instalado.
)

if not exist "C:\msys64\usr\bin\bash.exe" (
  echo ERRO: MSYS2 nao foi encontrado em C:\msys64.
  pause
  exit /b 1
)

echo [2/5] Preparando o compilador MinGW-w64...
"C:\msys64\usr\bin\bash.exe" -lc "pacman -Sy --noconfirm --needed mingw-w64-x86_64-gcc make"
if errorlevel 1 (
  echo ERRO: a instalacao do compilador C++ falhou.
  pause
  exit /b 1
)

set "PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"

where llama-server.exe >nul 2>nul
if errorlevel 1 (
  echo [3/5] Instalando o runtime C++ llama.cpp...
  winget install --exact --id ggml.llamacpp --accept-package-agreements --accept-source-agreements
  if errorlevel 1 (
    echo ERRO: nao foi possivel instalar llama.cpp.
    pause
    exit /b 1
  )
) else (
  echo [3/5] llama.cpp ja esta instalado.
)

set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;C:\msys64\mingw64\bin;C:\msys64\usr\bin;%PATH%"
where llama-server.exe >nul 2>nul
if errorlevel 1 (
  echo ERRO: llama-server.exe ainda nao apareceu no PATH.
  echo Feche esta janela, abra novamente este arquivo e tente outra vez.
  pause
  exit /b 1
)

echo [4/5] Compilando e executando 130 verificacoes...
call build-windows.cmd
if errorlevel 1 (
  echo ERRO: a compilacao ou os testes falharam.
  pause
  exit /b 1
)

echo [5/5] Abrindo Cortex X40 com Granite 4.2 3B e modo de sistema supervisionado...
echo O perfil usa pesos oficiais IBM Granite. O primeiro download GGUF tem aproximadamente 2,24 GB.
call start-windows-system.cmd
endlocal
