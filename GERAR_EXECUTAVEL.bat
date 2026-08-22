@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"
title Gerador do Roblox Fixer Launcher Pro

set "PYTHON_EXE=%LocalAppData%\Programs\Python\Python312\python.exe"
set "INSTALLER=%TEMP%\python-3.12.10-amd64.exe"
set "DOWNLOAD_URL=https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe"
set "OUTPUT=%~dp0dist\Roblox-Fixer-Launcher-Pro.exe"
set "WORK_DIR=%TEMP%\roblox-fixer-pyinstaller"

echo ============================================================
echo       Roblox Fixer Launcher Pro - Gerador de Executavel
echo ============================================================
echo.
echo Este processo baixa as ferramentas necessarias automaticamente.
echo Voce nao precisa ter Python ou Node.js instalado.
echo.

if exist "%PYTHON_EXE%" goto :python_ready

echo [1/4] Baixando o Python oficial...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ProgressPreference='SilentlyContinue'; try { Invoke-WebRequest -UseBasicParsing -Uri '%DOWNLOAD_URL%' -OutFile '%INSTALLER%' } catch { Write-Error $_; exit 1 }"
if errorlevel 1 goto :download_error
if not exist "%INSTALLER%" goto :download_error

echo [2/4] Instalando componentes de compilacao...
"%INSTALLER%" /quiet InstallAllUsers=0 PrependPath=0 Include_launcher=1 Include_pip=1 Include_tcltk=1 Include_test=0 Shortcuts=0
if errorlevel 1 goto :install_error
if not exist "%PYTHON_EXE%" goto :install_error

goto :dependencies

:python_ready
echo [1/4] Python de compilacao ja esta disponivel.
echo [2/4] Instalacao nao necessaria.

:dependencies
echo [3/4] Instalando CustomTkinter e PyInstaller...
"%PYTHON_EXE%" -m pip install --disable-pip-version-check --upgrade pip
if errorlevel 1 goto :dependency_error
"%PYTHON_EXE%" -m pip install --disable-pip-version-check -r "%~dp0requirements.txt"
if errorlevel 1 goto :dependency_error

echo [4/4] Gerando o arquivo EXE sem console...
if exist "%WORK_DIR%" rmdir /s /q "%WORK_DIR%"
"%PYTHON_EXE%" -m PyInstaller ^
  --noconsole ^
  --onefile ^
  --uac-admin ^
  --clean ^
  --noconfirm ^
  --name "Roblox-Fixer-Launcher-Pro" ^
  --collect-all customtkinter ^
  --distpath "%~dp0dist" ^
  --workpath "%WORK_DIR%" ^
  --specpath "%TEMP%" ^
  "%~dp0roblox_fixer_launcher.py"
if errorlevel 1 goto :build_error
if not exist "%OUTPUT%" goto :build_error

echo.
echo ============================================================
echo SUCESSO!
echo O executavel foi criado em:
echo %OUTPUT%
echo ============================================================
explorer.exe /select,"%OUTPUT%"
pause
exit /b 0

:download_error
echo.
echo ERRO: nao foi possivel baixar o Python oficial.
echo Confira sua conexao com a internet e tente novamente.
goto :failed

:install_error
echo.
echo ERRO: a instalacao automatica do Python falhou.
echo Tente executar este gerador novamente como administrador.
goto :failed

:dependency_error
echo.
echo ERRO: nao foi possivel baixar as dependencias.
echo Confira a internet, o antivirus e o firewall.
goto :failed

:build_error
echo.
echo ERRO: o PyInstaller nao conseguiu gerar o executavel.
goto :failed

:failed
echo.
echo Nenhuma alteracao manual de codigo e necessaria.
echo Pressione uma tecla para fechar e tente novamente depois.
pause >nul
exit /b 1
