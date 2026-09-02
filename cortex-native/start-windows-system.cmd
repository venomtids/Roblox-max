@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

fltmc >nul 2>nul
if errorlevel 1 (
  echo A Cortex solicitara ao Windows permissao administrativa para esta sessao.
  echo Nenhum arquivo sera alterado sem uma segunda confirmacao APLICAR na interface.
  powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

echo Permissao administrativa concedida pelo Windows.
echo O modo irrestrito aceita caminhos do sistema e comandos de validacao,
echo mas mostra cada operacao e exige APLICAR ou EXECUTAR antes de agir.
call start-windows.cmd --system
endlocal
