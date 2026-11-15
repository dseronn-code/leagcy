# Troca um agente antigo (sem autoatualização) pelo agente_rtx.ps1 atual, sem ninguém no PC.
# Vai como 1ª linha de um pedido. No agente novo (LEGACY_AGENTE_VERSAO definido) não faz nada.
# No antigo: abre o agente novo numa janela nova (herda o ambiente do Developer PowerShell, então cmake e git
# continuam funcionando; ele espera 20 s antes de começar) e encerra o agente antigo, que é o avô deste processo
# (agente -> cmd.exe -> este powershell). O agente novo então roda o pedido inteiro (esta linha vira nada).
if ($env:LEGACY_AGENTE_VERSAO) { Write-Output "agente já é o novo"; exit 0 }
$cmd = (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId
$agente = (Get-CimInstance Win32_Process -Filter "ProcessId=$cmd").ParentProcessId
$info = Get-CimInstance Win32_Process -Filter "ProcessId=$agente"
if (-not $info -or $info.CommandLine -notmatch 'agente_rtx\.ps1') { Write-Output "não achei o agente antigo ($($info.CommandLine)); nada feito"; exit 1 }
$script = Join-Path $PSScriptRoot "agente_rtx.ps1"
Start-Process powershell -WorkingDirectory (Split-Path $PSScriptRoot) -ArgumentList @('-NoExit', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"", '-Atraso', '20')
Write-Output "agente novo aberto numa janela nova; encerrando o antigo (pid $agente)"
Stop-Process -Id $agente -Force
