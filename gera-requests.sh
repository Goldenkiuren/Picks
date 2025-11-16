#!/usr/bin/env bash
set -eu

if [ $# -lt 2 ]; then
  echo "Uso: $0 <IP_DESTINO> <modo> [N] [OUTFILE]"
  echo "  <modo>: '1' => valor sempre 1; 'rand'|'random' => valor aleatório 0..100"
  echo "  [N]: número de linhas (padrão 10000)"
  echo "  [OUTFILE]: arquivo de saída (padrão requests.txt)"
  exit 1
fi

IP_DESTINO="$1"
MODE="$2"
N="${3:-10000}"
OUTFILE="${4:-requests.txt}"

# valida N é inteiro positivo
if ! [[ "$N" =~ ^[0-9]+$ ]] || [ "$N" -le 0 ]; then
  echo "ERRO: N deve ser inteiro positivo. Recebido: $N" >&2
  exit 2
fi

# valida modo
case "$MODE" in
  1) MODETYPE="fixed" ;;
  rand|random) MODETYPE="rand" ;;
  *) echo "ERRO: modo inválido. Use '1' ou 'rand'/'random'." >&2; exit 3 ;;
esac

# Gera arquivo: usa awk para performance e boa aleatoriedade
awk -v ip="$IP_DESTINO" -v mode="$MODETYPE" 'BEGIN { srand(); }
{
  if(mode=="fixed") v = 1;
  else v = int(rand()*101);    # 0..100 inclusive
  print ip, v;
}' <(seq 1 "$N") > "$OUTFILE"

echo "Gerado $OUTFILE com $N linhas (IP='$IP_DESTINO', modo='$MODE')."
