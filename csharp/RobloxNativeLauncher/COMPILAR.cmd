@echo off
setlocal
cd /d "%~dp0"
where dotnet >nul 2>&1
if errorlevel 1 (
  echo ERRO: o .NET 8 SDK nao foi encontrado.
  echo Instale apenas o SDK com: winget install Microsoft.DotNet.SDK.8
  pause
  exit /b 1
)

echo Compilando Roblox Native Launcher...
dotnet publish RobloxNativeLauncher.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
if errorlevel 1 (
  echo Falha na compilacao.
  pause
  exit /b 1
)

set "EXE=%~dp0bin\Release\net8.0-windows\win-x64\publish\RobloxNativeLauncher.exe"
echo.
echo Executavel criado em:
echo %EXE%
explorer.exe /select,"%EXE%"
pause
