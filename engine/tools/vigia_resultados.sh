#!/bin/bash
# Vigia barato dos testes do PC (roda aqui, no ambiente do Claude). Não acorda o Claude a cada mudança de status:
# só sai (e acorda) quando há algo para fazer:
#   RESULTADO NOVO <hash>   - o PC publicou resultados
#   ALERTA <status>         - o agente relatou travamento/limite/cancelamento
#   PC MUDO <min>           - o agente novo está rodando um comando mas parou de dar sinal de vida (manda a cada minuto)
#   FIM DO PRAZO            - passou o tempo máximo sem nada (o Claude decide se reinicia o vigia)
# Uso: tools/vigia_resultados.sh <arquivo com o último hash visto> [minutos máximos, padrão 115]
cd "$(dirname "$0")/.." || exit 1
F=$1; MAX=${2:-115}; t0=$(date +%s); alertado=""
while true; do
  git fetch -q origin resultados-rtx status-rtx 2>/dev/null || true
  r=$(git rev-parse origin/resultados-rtx 2>/dev/null)
  [ -n "$r" ] && [ "$r" != "$(cat "$F" 2>/dev/null)" ] && { echo "RESULTADO NOVO $r"; exit 0; }
  st=$(git show origin/status-rtx:status.txt 2>/dev/null | tail -1)
  if echo "$st" | grep -qiE "travad|passou de|cancelad|falhou" && [ "$st" != "$alertado" ]; then
    echo "ALERTA $st"; exit 0
  fi
  quando=$(echo "$st" | cut -c1-19); idade=$(( ($(date -u +%s) - $(date -u -d "$quando" +%s 2>/dev/null || date +%s)) / 60 ))
  if echo "$st" | grep -qE "rodando.* [0-9]+/[0-9]+ min" && [ "$idade" -ge 15 ]; then echo "PC MUDO ${idade} min: $st"; exit 0; fi
  [ $(( ($(date +%s) - t0) / 60 )) -ge "$MAX" ] && { echo "FIM DO PRAZO (último status: $st)"; exit 0; }
  sleep 60
done
