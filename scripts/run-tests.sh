#!/usr/bin/env bash
# run-tests.sh — enchaîne TOUS les tests du projet et rend un code de sortie.
#
# Lancé par `make test`. Trois étages :
#   1. les testeurs unitaires de chaque module (test_*) ;
#   2. la non-régression du parser sur les captures de samples/*.jsonl ;
#   3. un bout-en-bout simulateur → daemon, avec vérification des checksums
#      NMEA 0183 et du nombre de phrases produites.
#
# Aucun matériel requis. Usage : scripts/run-tests.sh [--verbose]
set -u

cd "$(dirname "$0")/.." || exit 2

VERBOSE=0
[ "${1:-}" = "--verbose" ] && VERBOSE=1

pass=0; fail=0
say()  { printf '%s\n' "$*"; }
ok()   { pass=$((pass+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
ko()   { fail=$((fail+1)); printf '  \033[31mKO\033[0m   %s\n' "$1"; }

run_case() {                      # run_case <nom> <commande...>
  local name=$1; shift
  local out
  # stdin fermé : plusieurs testeurs (test_jsonl, test_registry) LISENT stdin et
  # attendraient indéfiniment s'ils héritaient du terminal.
  if out=$("$@" 2>&1 </dev/null); then
    ok "$name"
    [ "$VERBOSE" = 1 ] && printf '%s\n' "$out" | sed 's/^/       /'
  else
    ko "$name (code $?)"
    printf '%s\n' "$out" | tail -15 | sed 's/^/       /'
  fi
}

# ---- 1. testeurs unitaires ------------------------------------------------
say "== testeurs unitaires =="
UNITS="test_registry test_nmea0183 test_config test_arbiter test_mapper
       test_aisdedup test_sources test_stats test_netout test_ydraw
       test_inimerge"
for t in $UNITS; do
  if [ -x "./$t" ]; then
    run_case "$t" "./$t"
  else
    ko "$t absent (make d'abord)"
  fi
done

# ---- 2. non-régression du parser sur les captures -------------------------
say "== captures (samples/*.jsonl) =="
if [ ! -x ./test_jsonl ]; then
  ko "test_jsonl absent (make d'abord)"
else
  shopt -s nullglob
  caps=(samples/*.jsonl)
  if [ ${#caps[@]} -eq 0 ]; then
    say "  (aucune capture dans samples/ — voir samples/README.md)"
  fi
  for f in "${caps[@]}"; do
    # test_jsonl sort non nul dès qu'UNE ligne ne se parse pas.
    run_case "$(basename "$f")" sh -c "./test_jsonl < '$f' > /dev/null"
  done
fi

# ---- 3. bout-en-bout simulateur → daemon ----------------------------------
say "== bout-en-bout (n2k-sim → n2k-mux) =="
if [ ! -x ./n2k-sim ] || [ ! -x ./n2k-mux ]; then
  ko "n2k-sim ou n2k-mux absent (make d'abord)"
else
  out0183=$(./n2k-sim --once 2>/dev/null | ./n2k-mux n2k-sim.ini 2>/dev/null)
  if [ -z "$out0183" ]; then
    ko "aucune phrase 0183 produite"
  else
    ok "phrases produites : $(printf '%s\n' "$out0183" | grep -c '^\$')"
    # Checksum XOR de chaque phrase : c'est l'invariant le moins cher et le
    # plus parlant (une phrase mal formée le casse presque toujours).
    if printf '%s\n' "$out0183" | python3 -c '
import sys
bad = 0; n = 0
for line in sys.stdin:
    line = line.strip()
    if not line or line[0] not in "$!":
        continue
    if "*" not in line:
        print("pas de checksum :", line); bad += 1; continue
    body, _, ck = line[1:].partition("*")
    x = 0
    for c in body.encode():
        x ^= c
    n += 1
    if ck.strip().upper() != "%02X" % x:
        print("checksum faux :", line, "attendu %02X" % x); bad += 1
if bad:
    sys.exit(1)
print("checksums vérifiés :", n)
'; then
      ok "checksums 0183"
    else
      ko "checksums 0183"
    fi
    # Quelques phrases clés doivent être là : elles couvrent position, route,
    # temps, satellites, cap, vent, profondeur et météo.
    for want in GLL VTG RMC GGA GSV HDG MWV DPT MDA MTW VLW ROT RSA VHW; do
      if printf '%s\n' "$out0183" | grep -q "^\$II$want"; then
        ok "phrase $want"
      else
        ko "phrase $want absente"
      fi
    done
  fi
fi

say ""
say "Total : $pass ok, $fail KO"
[ "$fail" -eq 0 ] || exit 1
