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
#
# Sinal de vida: a cada 10 min (e a cada etapa) o agente publica engine/status.txt no branch "status-rtx" com a hora e
# o que está fazendo; com um comando rodando, a cada minuto (tempo decorrido e última linha da saída).
# Vigia: cada linha do pedido pode dizer quanto deve demorar ("[5] comando" = uns 5 min, limite = o dobro; sem isso,
# 20 min). Se passar do limite, ou ficar 6 min sem saída nova e sem usar CPU (travado), o agente grava um diagnóstico
# (fim da saída, processos, GPU, memória), encerra o comando e tenta de novo uma vez; se falhar de novo, segue.
# Se o pedido mudar no GitHub no meio da rodada, ela é cancelada e a nova começa. Se este próprio arquivo mudar no
# GitHub, o agente se reinicia sozinho com a versão nova.
param(
  [string]$Branch = "ccr-5423dd89-y3br6e",
  [int]$Intervalo = 30,
  [int]$Atraso = 0   # segundos de espera ao ligar (usado quando o agente antigo se troca por este)
)
$env:LEGACY_AGENTE_VERSAO = "2"   # os comandos do pedido sabem que estão no agente novo (ver tools/atualizar_agente.ps1)
if ($Atraso -gt 0) { Start-Sleep $Atraso }

$Pasta = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path   # a pasta engine
$Estado = Join-Path $env:LOCALAPPDATA "legacy_agente_rtx.txt"  # último pedido rodado (fora do repositório)
$EstadoComando = Join-Path $env:LOCALAPPDATA "legacy_agente_comando.txt"  # último comando do PC atendido
$env:PYTHONIOENCODING = "utf-8"
Set-Location $Pasta

function Agora { (Get-Date).ToString("HH:mm:ss") }

$script:UltimoSinal = [datetime]::MinValue
function SinalDeVida([string]$Estado, [switch]$Forcar) {
  if (-not $Forcar -and ((Get-Date) - $script:UltimoSinal).TotalMinutes -lt 10) { return }
  $script:UltimoSinal = Get-Date
  $dir = Join-Path $env:TEMP "legacy_status_rtx"
  if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
  New-Item -ItemType Directory $dir | Out-Null
  "$((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC | $env:COMPUTERNAME | $Estado" |
    Out-File -Encoding ascii "$dir\status.txt"
  $url = (git -C $Pasta remote get-url origin)
  Push-Location $dir
  git init -q
  git checkout -q -b status
  git add -A
  git -c user.name=agente-rtx -c user.email=agente-rtx@localhost commit -q -m "status" 2>$null | Out-Null
  git push -q --force $url HEAD:refs/heads/status-rtx 2>$null
  Pop-Location
}

# Escreve uma linha na saída. Logo depois de matar um comando, o Windows pode levar alguns segundos para soltar o
# arquivo (o programa morto ainda o segurava): tenta por até 30 s e, se não der, grava em anotacoes.txt.
function Anotar([string]$Arquivo, [string]$Texto) {
  for ($t = 0; $t -lt 30; $t++) {
    try { [IO.File]::AppendAllText($Arquivo, $Texto); return } catch { Start-Sleep 1 }
  }
  try { [IO.File]::AppendAllText("$Pasta\resultados_pc\anotacoes.txt", $Texto) } catch { }
}

# Roda um comando do pedido vigiando-o (a saída vai para resultados_pc\saida.txt). Devolve:
#   o código de saída do programa; -999 = passou do limite; -998 = travado (sem saída nova e sem usar CPU por
#   $MinTravado min); -997 = o pedido mudou no GitHub no meio (rodada cancelada para rodar a nova).
# A cada minuto publica o sinal de vida com o tempo decorrido e a última linha da saída, para quem está longe ver
# exatamente onde está. Quando encerra por limite ou trava, grava um diagnóstico (fim da saída, processos, GPU,
# memória) em resultados_pc\diagnostico_N.txt antes de matar o processo.
$MinTravado = 6
function Diagnostico([string]$Motivo, [int]$N, $Proc) {
  $f = "$Pasta\resultados_pc\diagnostico_$N.txt"
  "=== $Motivo ($(Agora)) ===" | Out-File -Encoding utf8 $f
  "--- fim da saída ---" | Out-File -Append -Encoding utf8 $f
  Get-Content "$Pasta\resultados_pc\saida.txt" -Tail 60 -ErrorAction SilentlyContinue | Out-File -Append -Encoding utf8 $f
  "--- processos do comando ---" | Out-File -Append -Encoding utf8 $f
  Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.ParentProcessId -eq $Proc.Id -or $_.ProcessId -eq $Proc.Id } |
    ForEach-Object { "{0} pid={1} mem={2:N0} MB" -f $_.Name, $_.ProcessId, ($_.WorkingSetSize / 1MB) } | Out-File -Append -Encoding utf8 $f
  Get-Process legacy, python, node -ErrorAction SilentlyContinue |
    ForEach-Object { "{0} pid={1} cpu={2:N0}s mem={3:N0} MB respondendo={4}" -f $_.Name, $_.Id, $_.CPU, ($_.WorkingSet64 / 1MB), $_.Responding } |
    Out-File -Append -Encoding utf8 $f
  "--- GPU ---" | Out-File -Append -Encoding utf8 $f
  cmd /c "nvidia-smi --query-gpu=name,temperature.gpu,utilization.gpu,memory.used,memory.total --format=csv 2>&1" | Out-File -Append -Encoding utf8 $f
  "--- memória livre ---" | Out-File -Append -Encoding utf8 $f
  $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
  if ($os) { "{0:N0} MB livres de {1:N0} MB" -f ($os.FreePhysicalMemory / 1KB), ($os.TotalVisibleMemorySize / 1KB) | Out-File -Append -Encoding utf8 $f }
}

function CpuDaArvore($Proc) {
  # CPU somada do comando e de todos os descendentes (python -> node -> electron-builder, etc.).
  $todos = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
  $ids = @($Proc.Id); $k = 0
  while ($k -lt $ids.Count) {
    $pai = $ids[$k]; $k++
    $ids += @($todos | Where-Object { $_.ParentProcessId -eq $pai -and $ids -notcontains $_.ProcessId } | ForEach-Object { $_.ProcessId })
  }
  $t = 0.0
  foreach ($i in $ids) { $q = Get-Process -Id $i -ErrorAction SilentlyContinue; if ($q -and $q.CPU) { $t += $q.CPU } }
  return $t
}

function RodarComLimite([string]$Comando, [int]$Minutos, [int]$N, [string]$Rotulo, [string]$Pedido) {
  $saidaTxt = "$Pasta\resultados_pc\saida.txt"
  $p = Start-Process -FilePath cmd.exe -ArgumentList "/c $Comando >> resultados_pc\saida.txt 2>&1" -NoNewWindow -PassThru
  $null = $p.Handle  # sem isso o ExitCode pode vir vazio no Windows PowerShell
  $inicio = Get-Date
  $ultTam = -1; $ultCpu = -1.0; $parado = Get-Date
  while (-not $p.WaitForExit(60 * 1000)) {
    $min = [int]((Get-Date) - $inicio).TotalMinutes
    $tam = (Get-Item $saidaTxt -ErrorAction SilentlyContinue).Length
    $cpu = CpuDaArvore $p
    if ($tam -ne $ultTam -or [math]::Abs($cpu - $ultCpu) -gt 2.0) { $parado = Get-Date }
    $ultTam = $tam; $ultCpu = $cpu
    $linha = (Get-Content $saidaTxt -Tail 1 -ErrorAction SilentlyContinue)
    if ($linha) { $linha = $linha.Substring(0, [math]::Min(80, $linha.Length)) }
    SinalDeVida "$Rotulo $min/$Minutos min | $linha" -Forcar
    $motivo = $null; $code = 0
    if ($min -ge $Minutos) { $motivo = "passou de $Minutos min"; $code = -999 }
    elseif (((Get-Date) - $parado).TotalMinutes -ge $MinTravado) { $motivo = "travado: $MinTravado min sem saída nova e sem usar CPU"; $code = -998 }
    else {
      git fetch -q origin $Branch 2>$null
      $agora = (git rev-parse "origin/${Branch}:engine/pedido_rtx.txt" 2>$null)
      if ($agora -and $agora -ne $Pedido) { $motivo = "pedido novo no GitHub: rodada cancelada"; $code = -997 }
    }
    if ($motivo) {
      Write-Host "[$(Agora)] $motivo; encerrando"
      Diagnostico $motivo $N $p
      cmd /c "taskkill /T /F /PID $($p.Id)" | Out-Null
      return $code
    }
  }
  return $p.ExitCode
}

# Tempo esperado de uma linha do pedido: "[5] comando" quer dizer "uns 5 min"; o limite é o dobro (mínimo 4 min).
# Sem o prefixo, o limite é 20 min.
function LimiteDe([string]$Linha) {
  if ($Linha -match '^\s*\[(\d+)\]\s*(.*)$') { return @([math]::Max(4, 2 * [int]$Matches[1]), $Matches[2]) }
  return @(20, $Linha.Trim())
}

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
  # Mensagem por arquivo: aspas no assunto do commit (ex.: "Não está respondendo") quebravam o -m no PowerShell.
  $msg = Join-Path $env:TEMP "legacy_resultados_msg.txt"
  "Resultados da RTX ($ResumoCommit)" | Out-File -Encoding utf8 $msg
  git -c user.name=agente-rtx -c user.email=agente-rtx@localhost commit -q -F $msg
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
SinalDeVida "agente ligado" -Forcar
while ($true) {
  git fetch -q origin $Branch 2>$null
  if ($LASTEXITCODE -ne 0) { Write-Host "[$(Agora)] sem conexão com o GitHub; tento de novo"; Start-Sleep $Intervalo; continue }
  ComandoDoPc
  SinalDeVida "esperando pedido"
  $pedido = (git rev-parse "origin/${Branch}:engine/pedido_rtx.txt" 2>$null)
  $ultimo = if (Test-Path $Estado) { (Get-Content $Estado -Raw).Trim() } else { "" }
  if (-not $pedido -or $pedido -eq $ultimo) { Start-Sleep $Intervalo; continue }

  $commit = (git log -1 --format="%h %s" "origin/$Branch")
  $cancelada = $false
  Write-Host "[$(Agora)] Pedido novo: $commit"
  if (Test-Path resultados_pc) { Remove-Item -Recurse -Force resultados_pc }
  New-Item -ItemType Directory resultados_pc | Out-Null
  $saida = "$Pasta\resultados_pc\saida.txt"

  $antes = (Get-FileHash $PSCommandPath).Hash
  git checkout -q $Branch
  git merge -q --ff-only "origin/$Branch"
  if ($LASTEXITCODE -eq 0 -and (Get-FileHash $PSCommandPath).Hash -ne $antes) {
    Write-Host "[$(Agora)] O agente foi atualizado no GitHub: reiniciando com a versão nova"
    SinalDeVida "agente atualizado; reiniciando" -Forcar
    & powershell -ExecutionPolicy Bypass -File $PSCommandPath -Branch $Branch -Intervalo $Intervalo
    exit
  }
  if ($LASTEXITCODE -ne 0) {
    "!!! A pasta tem mudanças locais ou divergiu do GitHub: não atualizei nem rodei nada." | Out-File -Encoding utf8 $saida
    $naoMarcar = $true  # tenta de novo depois que a pasta for arrumada (não perde o pedido)
    git status --short | Out-File -Append -Encoding utf8 $saida
  } else {
    Write-Host "[$(Agora)] compilando..."
    cmd /c "cmake --build build --config Release > resultados_pc\compilacao.txt 2>&1"
    if ($LASTEXITCODE -ne 0) {
      "!!! A compilação falhou (veja compilacao.txt)." | Out-File -Encoding utf8 $saida
    } else {
      $comandos = Get-Content -Encoding UTF8 pedido_rtx.txt | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith("#") }
      $i = 0
      $cancelada = $false
      foreach ($linhaPedido in $comandos) {
        $i++
        $lim, $c = LimiteDe $linhaPedido
        $rotulo = "rodando $($commit.Substring(0, 7)) ($i/$($comandos.Count))"
        Write-Host "[$(Agora)] ($i/$($comandos.Count)) [limite $lim min] $c"
        SinalDeVida $rotulo -Forcar
        Anotar $saida "`n>>> $c`n"
        $code = RodarComLimite $c $lim $i $rotulo $pedido
        if ($code -eq -997) { Anotar $saida "!!! pedido novo no GitHub: rodada cancelada`n"; $cancelada = $true; break }
        if ($code -eq -999 -or $code -eq -998) {
          # Falhou por tempo: tenta uma segunda vez (descarta travamento de uma vez só, ex.: driver, disco, antivírus).
          $motivo = if ($code -eq -999) { "passou de $lim min" } else { "travou (sem saída nem CPU por $MinTravado min)" }
          Anotar $saida "!!! $motivo; diagnóstico em diagnostico_$i.txt; tentando de novo`n>>> (2ª tentativa) $c`n"
          $code = RodarComLimite $c $lim (100 + $i) "$rotulo 2a tentativa" $pedido
          if ($code -eq -997) { Anotar $saida "!!! pedido novo no GitHub: rodada cancelada`n"; $cancelada = $true; break }
          if ($code -eq -999 -or $code -eq -998) { Anotar $saida "!!! falhou de novo da mesma forma (diagnostico_$(100 + $i).txt); seguindo para o próximo`n" }
          elseif ($code -ne 0) { Anotar $saida "!!! código $code`n" }
          else { Anotar $saida "(2ª tentativa ok)`n" }
        }
        elseif ($code -ne 0) { Anotar $saida "!!! código $code`n" }
      }
    }
  }
  if ($cancelada) {
    # Publica o parcial e NÃO marca o pedido como rodado; a volta seguinte do laço pega o pedido novo.
    Publicar "$commit (cancelada: pedido novo)" | Out-Null
    continue
  }
  if (Publicar $commit) { Write-Host "[$(Agora)] Resultados enviados." } else { Write-Host "[$(Agora)] Falha ao enviar os resultados." }
  if ($naoMarcar) { $naoMarcar = $false; Start-Sleep 300; continue }  # avisa 1 vez a cada 5 min, sem perder o pedido
  Set-Content $Estado $pedido
}
