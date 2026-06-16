#!/usr/bin/env bash
# n2k-mux-test.sh — monte le pipeline complet POUR TEST, sans rien installer.
#
#   NGX-1 (Transfer) → actisense-serial → analyzer -json -nv
#     → tee ─┬→ n2k-mux --ais-json → n2kd      (AIS → TCP 2599)
#            └→ n2k-mux (instruments)
#     → kplex (foreground : stdin + tcp 2599)  → TCP 127.0.0.1:10110 + log /tmp
#
# Tout en espace utilisateur (FIFO + log dans /tmp, pas de /run ni /var/log,
# pas de sudo si tu es dans le groupe dialout). Ctrl-C arrête et nettoie.
#
# Usage : scripts/n2k-mux-test.sh [config.ini]
# Variables surchargeables : DEVICE BAUD CANBOAT N2KMUX KPLEX CONF
set -u

# --- chemins (adaptables par variables d'env) ---
DEVICE=${DEVICE:-/dev/ttyNGX1}
BAUD=${BAUD:-230400}
CANBOAT=${CANBOAT:-$(ls -d "$HOME"/canboat/rel/*/ 2>/dev/null | head -1)}
ACTISENSE=${ACTISENSE:-${CANBOAT}actisense-serial}
ANALYZER=${ANALYZER:-${CANBOAT}analyzer}
N2KD=${N2KD:-${CANBOAT}n2kd}
N2KMUX=${N2KMUX:-$HOME/n2k-mux/n2k-mux}
KPLEX=${KPLEX:-kplex}
CONF=${1:-${CONF:-/tmp/n2kmux.ini}}

TX=/tmp/n2k-mux-test-tx.fifo
AIS=/tmp/n2k-mux-test-ais.fifo
SOURCES=/tmp/n2k-mux-test-sources.json
KCONF=/tmp/n2k-mux-test-kplex.conf
LOG=/tmp/n2k-mux-test.log

die() { echo "ERREUR: $*" >&2; exit 1; }

# --- vérifications ---
[ -e "$DEVICE" ]      || die "device introuvable: $DEVICE"
[ -r "$DEVICE" ]      || die "pas d'accès à $DEVICE (groupe dialout ? sudo ?)"
[ -x "$ACTISENSE" ]   || die "actisense-serial introuvable: $ACTISENSE"
[ -x "$ANALYZER" ]    || die "analyzer introuvable: $ANALYZER"
[ -x "$N2KD" ]        || die "n2kd introuvable: $N2KD"
[ -x "$N2KMUX" ]      || die "n2k-mux introuvable: $N2KMUX (fais 'make' ?)"
command -v "$KPLEX" >/dev/null || die "kplex introuvable dans le PATH"
[ -f "$CONF" ]        || die "config introuvable: $CONF"

command -v ss >/dev/null && ss -ltn 2>/dev/null | grep -qE ':(2599|10110)\b' && \
  echo "ATTENTION: 2599 ou 10110 déjà en écoute (un kplex/n2kd tourne déjà ?)" >&2

# --- conf kplex de test (foreground, sorties dans /tmp, pas de /var/log) ---
cat > "$KCONF" <<EOF
[global]
mode=foreground
[file]
direction=in
filename=-
[tcp]
direction=in
mode=client
address=127.0.0.1
port=2599
persist=fromstart
retry=5
[tcp]
mode=server
port=10110
direction=out
[file]
direction=out
filename=$LOG
append=yes
EOF

# --- fifos ---
rm -f "$TX" "$AIS"
mkfifo "$TX" "$AIS" || die "mkfifo a échoué"

AIS_SID=""
cleanup() {
    echo; echo "arrêt…"
    [ -n "$AIS_SID" ] && kill -- -"$AIS_SID" 2>/dev/null
    rm -f "$TX" "$AIS"
}
trap cleanup EXIT INT TERM

cat <<EOF
=== n2k-mux — test ===
device   : $DEVICE @ $BAUD   (NGX-1 doit être en mode TRANSFER)
config   : $CONF
sortie   : qtVlm → TCP 127.0.0.1:10110     |   log : $LOG
sources  : $SOURCES (vues sur le bus)      |   AIS : n2kd TCP 2599
Ctrl-C pour arrêter.
EOF

# Branche AIS dans sa propre session (killable proprement) : lit une copie du
# flux analyzer (via le fifo $AIS), déduplique, encode en VDM.
setsid bash -c '"$1" --ais-json "$2" < "$3" | "$4"' _ \
    "$N2KMUX" "$CONF" "$AIS" "$N2KD" &
AIS_SID=$!

# Pipeline principal (premier plan) : acquisition → tee(copie AIS) → arbitrage → kplex.
"$ACTISENSE" -s "$BAUD" "$DEVICE" < "$TX" \
  | "$ANALYZER" -json -nv \
  | tee "$AIS" \
  | "$N2KMUX" "$CONF" --tx "$TX" --tx-interval 10 --sources "$SOURCES" \
  | "$KPLEX" -f "$KCONF"
