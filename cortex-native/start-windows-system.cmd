@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

echo A Cortex pedira permissao UAC ao Windows por meio do proprio executavel C++.
echo Nenhum arquivo sera alterado sem uma segunda confirmacao APLICAR na interface.
echo Comandos de validacao tambem exigem a confirmacao EXECUTAR.
call start-windows.cmd --system
endlocal
