# Agente de testes na RTX: roda os pedidos de engine/pedido_rtx.txt sem ninguém na frente do PC.
#
# A cada 30 s ele busca o branch de trabalho no GitHub. Quando o pedido muda, ele atualiza a pasta do motor (só avanço
# rápido, nunca apaga mudança local), compila, roda cada linha do pedido na pasta engine e publica a saída, o log da
# compilação e as imagens de resultados_pc no branch "resultados-rtx". Usa o git do PC com as credenciais de sempre,
# sem GitHub Actions (funciona com o repositório privado e não depende de cobrança).
#
# Uso (na pasta engine, numa "Developer PowerShell for VS 2022"):
#   powershell -ExecutionPolicy Bypass -File tools\agente_rtx.ps1
# Para parar: Ctrl+C. Ao ligar de novo, ele só roda pedidos novos.
#
# Controle do PC pelo celular: engine/comando_pc.txt com a palavra "desligar", "reiniciar" ou "cancelar" na primeira
# linha (as linhas com # são ignoradas). Cada mudança do arquivo vale uma vez. O desligamento é o normal do Windows,
# com 60 s de aviso na tela ("cancelar", ou shutdown /a no PC, desiste).
param(
  [string]$Branch = "ccr-5423dd89-y3br6e",
  [int]$Intervalo = 30
)

$Pasta = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path   # a pasta engine
$Estado = Join-Path $env:LOCALAPPDATA "legacy_agente_rtx.txt"  # último pedido rodado (fora do repositório)
$EstadoComando = Join-Path $env:LOCALAPPDATA "legacy_agente_comando.txt"  # último comando do PC atendido
$env:PYTHONIOENCODING = "utf-8"
Set-Location $Pasta

function Agora { (Get-Date).ToString("HH:mm:ss") }

function Publicar([string]$ResumoCommit) {
  $out = Join-Path $env:TEMP "legacy_resultados_rtx"
  if (Test-Path $out) { Remove-Item -Recurse -Force $out }
  New-Item -ItemType Directory $out | Out-Null
  if (Test-Path "$Pasta\resultados_pc") { Copy-Item -Recurse "$Pasta\resultados_pc\*" $out }
  Copy-Item "$Pasta\pedido_rtx.txt" $out -ErrorAction SilentlyContinue
  $ResumoCommit | Out-File -Encoding ascii "$out\commit.txt"
  $url = (git -C $Pasta remote get-url origin)
  Push-Location $out
  git init -q
  git checkout -q -b resultados
  git add -A
  git -c user.name=agente-rtx -c user.email=agente-rtx@localhost commit -q -m "Resultados da RTX ($ResumoCommit)"
  git push -q --force $url HEAD:refs/heads/resultados-rtx
  $ok = $LASTEXITCODE -eq 0
  Pop-Location
  return $ok
}

function ComandoDoPc {
  $hash = (git rev-parse "origin/${Branch}:engine/comando_pc.txt" 2>$null)
  if (-not $hash) { return }
  if (-not (Test-Path $EstadoComando)) { Set-Content $EstadoComando $hash; return }  # 1ª vez: só anota, não executa
  if ((Get-Content $EstadoComando -Raw).Trim() -eq $hash) { return }
  Set-Content $EstadoComando $hash  # antes de executar: depois de reiniciar, o mesmo comando não vale de novo
  $linha = (git show "origin/${Branch}:engine/comando_pc.txt" 2>$null) | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith("#") } |
           Select-Object -First 1
  $cmd = if ($linha) { $linha.Trim().ToLower() } else { "" }
  switch ($cmd) {
    "desligar"  { Write-Host "[$(Agora)] Comando do celular: desligar em 60 s"; shutdown /s /t 60 /c "Legacy: desligamento pedido pelo celular (shutdown /a cancela)" }
    "reiniciar" { Write-Host "[$(Agora)] Comando do celular: reiniciar em 60 s"; shutdown /r /t 60 /c "Legacy: reinício pedido pelo celular (shutdown /a cancela)" }
    "cancelar"  { Write-Host "[$(Agora)] Comando do celular: cancelar"; shutdown /a }
    default     { Write-Host "[$(Agora)] Comando do celular ignorado: '$cmd'" }
  }
}

Write-Host "[$(Agora)] Agente da RTX ligado em $Pasta (branch $Branch). Ctrl+C para parar."
while ($true) {
  git fetch -q origin $Branch 2>$null
  if ($LASTEXITCODE -ne 0) { Write-Host "[$(Agora)] sem conexão com o GitHub; tento de novo"; Start-Sleep $Intervalo; continue }
  ComandoDoPc
  $pedido = (git rev-parse "origin/${Branch}:engine/pedido_rtx.txt" 2>$null)
  $ultimo = if (Test-Path $Estado) { (Get-Content $Estado -Raw).Trim() } else { "" }
  if (-not $pedido -or $pedido -eq $ultimo) { Start-Sleep $Intervalo; continue }

  $commit = (git log -1 --format="%h %s" "origin/$Branch")
  Write-Host "[$(Agora)] Pedido novo: $commit"
  if (Test-Path resultados_pc) { Remove-Item -Recurse -Force resultados_pc }
  New-Item -ItemType Directory resultados_pc | Out-Null
  $saida = "$Pasta\resultados_pc\saida.txt"

  git checkout -q $Branch
  git merge -q --ff-only "origin/$Branch"
  if ($LASTEXITCODE -ne 0) {
    "!!! A pasta tem mudanças locais ou divergiu do GitHub: não atualizei nem rodei nada." | Out-File -Encoding utf8 $saida
    git status --short | Out-File -Append -Encoding utf8 $saida
  } else {
    Write-Host "[$(Agora)] compilando..."
    cmd /c "cmake --build build --config Release > resultados_pc\compilacao.txt 2>&1"
    if ($LASTEXITCODE -ne 0) {
      "!!! A compilação falhou (veja compilacao.txt)." | Out-File -Encoding utf8 $saida
    } else {
      $comandos = Get-Content -Encoding UTF8 pedido_rtx.txt | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith("#") }
      $i = 0
      foreach ($c in $comandos) {
        $i++
        Write-Host "[$(Agora)] ($i/$($comandos.Count)) $c"
        [IO.File]::AppendAllText($saida, "`n>>> $c`n")
        cmd /c "$c >> resultados_pc\saida.txt 2>&1"
        if ($LASTEXITCODE -ne 0) { [IO.File]::AppendAllText($saida, "!!! código $LASTEXITCODE`n") }
      }
    }
  }
  if (Publicar $commit) { Write-Host "[$(Agora)] Resultados enviados." } else { Write-Host "[$(Agora)] Falha ao enviar os resultados." }
  Set-Content $Estado $pedido
}
