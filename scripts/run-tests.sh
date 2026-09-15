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
       test_inimerge test_polar"
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
    # Cohérence du cap : le cap vrai tiré de HDG (magnétique + déviation +
    # variation) doit égaler HDT. Une déviation annoncée mais non appliquée les
    # a déjà décalés de 1,5°.
    if printf '%s\n' "$out0183" | python3 -c '
import sys
hdg = hdt = None
for l in sys.stdin:
    f = l.split("*")[0].split(",")
    if f[0].endswith("HDG"):
        dev = float(f[2] or 0) * (1 if f[3] == "E" else -1)
        var = float(f[4] or 0) * (1 if f[5] == "E" else -1)
        hdg = (float(f[1]) + dev + var) % 360
    elif f[0].endswith("HDT"):
        hdt = float(f[1])
sys.exit(0 if hdg is not None and hdt is not None and abs((hdg - hdt + 180) % 360 - 180) < 0.15 else 1)
'; then ok "cap vrai de HDG = HDT"; else ko "cap vrai de HDG différent de HDT"; fi
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

# ---- 3 bis. perdants publiés pour n2k-filter --------------------------------
say "== perdants (--losers) =="
if [ ! -x ./n2k-mux ]; then
  ko "n2k-mux absent (make d'abord)"
else
  LD=$(mktemp -d)
  # A gagne « Apparent » mais perd « True », B l'inverse : aucun des deux ne
  # doit être jeté, n2k-filter ne voyant que (pgn, src). En position, B perd
  # partout : lui seul est publié.
  printf '[sources]\nA = SER_A\nB = SER_B\n\n[priority]\n130306/Apparent = A, B\n130306/True = B, A\n129025 = A, B\n' > "$LD/l.ini"
  {
    for s in 10:SER_A 11:SER_B; do
      printf '{"src":%s,"pgn":126996,"fields":{"Model Serial Code":"%s"}}\n' "${s%%:*}" "${s#*:}"
    done
    for i in 1 2 3; do
      for src in 10 11; do
        printf '{"src":%d,"pgn":130306,"fields":{"Reference":"Apparent","Wind Angle":30}}\n' "$src"
        printf '{"src":%d,"pgn":130306,"fields":{"Reference":"True (water referenced)","Wind Angle":40}}\n' "$src"
        printf '{"src":%d,"pgn":129025,"fields":{"Latitude":47.5,"Longitude":-3.0}}\n' "$src"
      done
    done
  } | ./n2k-mux "$LD/l.ini" --losers "$LD/losers.txt" >/dev/null 2>&1
  got=$(grep -v '^#' "$LD/losers.txt" 2>/dev/null | sort | tr '\n' ' ')
  if [ "$got" = "129025 11 " ]; then ok "perdants : discriminants gagnants non jetés"
  else ko "perdants attendus « 129025 11 », obtenu « $got »"; fi
  rm -rf "$LD"
fi

# ---- 4. simulateur piloté (fichier de contrôle) ---------------------------
say "== simulateur piloté (--control) =="
if [ ! -x ./n2k-sim ]; then
  ko "n2k-sim absent (make d'abord)"
else
  CTL=$(mktemp); STATE=$(mktemp)
  # Six entrées : cap 45, 6 nds surface, courant 2 nds portant à l'est, vent
  # vrai du 105 à 20 nds. 6 nds = 3,09 m/s. Route fond = surface + courant :
  # 55,8° à 7,55 nds. TWA = TWD − HDG = 60 ; apparent calculé : 47,1° à 25,6 nds.
  printf 'enabled = 1\nhdg = 45\nstw = 6\nset = 90\ndrift = 2\ntwd = 105\ntws = 20\n' > "$CTL"
  SIMOUT=$(mktemp)
  ./n2k-sim --once --control "$CTL" --state "$STATE" > "$SIMOUT" 2>/dev/null
  run_case "cap imposé"            grep -q '"Heading":45.0,"Reference":"True"' "$SIMOUT"
  run_case "vitesse surface imposée" grep -q '"Speed Water Referenced":3.09' "$SIMOUT"
  run_case "route fond déduite"    grep -q '"COG":55.8' "$SIMOUT"
  run_case "vitesse fond déduite"  grep -q '"SOG":3.88' "$SIMOUT"
  run_case "TWA déduit"            grep -q '^twa = 60.0$' "$STATE"
  run_case "AWA déduit"            grep -q '^awa = 47.1$' "$STATE"
  run_case "AWS déduit"            grep -q '^aws = 25.58$' "$STATE"
  # twa/awa/aws ne sont PAS des entrées : une clé parasite doit être ignorée.
  printf 'enabled = 1\nhdg = 45\nstw = 6\nset = 90\ndrift = 2\ntwd = 105\ntws = 20\nawa = 10\n' > "$CTL"
  ./n2k-sim --once --control "$CTL" --state "$STATE" > /dev/null 2>&1
  run_case "AWA non réglable"      grep -q '^awa = 47.1$' "$STATE"
  rm -f "$SIMOUT" "$STATE"
  # Porte « enabled » : plus rien que l'en-tête analyzer.
  printf 'enabled = 0\n' > "$CTL"
  n=$(./n2k-sim --duration 1 --control "$CTL" 2>/dev/null | grep -c '"pgn"' || true)
  if [ "$n" = "0" ]; then ok "désactivé : aucun PGN émis"; else ko "désactivé : $n PGN émis"; fi
  rm -f "$CTL"
fi

# ---- 4 bis. vent aléatoire et polaire ---------------------------------------
say "== vent aléatoire et polaire =="
if [ ! -x ./n2k-sim ]; then
  ko "n2k-sim absent (make d'abord)"
else
  TD=$(mktemp -d)
  # Aléa : base 225° / 15 nds, amplitudes TOTALES 20 % (le maximum) et 20°,
  # graine fixe.
  # Deux heures de temps simulé, sans attendre (--wind-trace).
  printf 'enabled = 1\nhdg = 45\ntwd = 225\ntws = 15\nwind_random = 1\ntws_var = 20\ntwd_var = 20\nwind_period = 10\nseed = 7\n' > "$TD/r.ctl"
  ./n2k-sim --wind-trace 7200 --control "$TD/r.ctl" > "$TD/a.csv" 2>/dev/null
  ./n2k-sim --wind-trace 7200 --control "$TD/r.ctl" > "$TD/b.csv" 2>/dev/null
  run_case "aléa reproductible avec une graine" cmp -s "$TD/a.csv" "$TD/b.csv"
  if python3 - "$TD/a.csv" <<'PYCHK'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1]), delimiter=';'))
tws = [float(r['tws_kn']) for r in rows]
dev = [((float(r['twd']) - 225 + 180) % 360) - 180 for r in rows]
ok = True
if not (15 * 0.90 - 1e-6 <= min(tws) and max(tws) <= 15 * 1.10 + 1e-6):
    print("force hors amplitude :", min(tws), max(tws)); ok = False
if not (-10 - 1e-6 <= min(dev) and max(dev) <= 10 + 1e-6):
    print("direction hors amplitude :", min(dev), max(dev)); ok = False
if max(tws) - min(tws) < 1.0 or max(dev) - min(dev) < 2.0:
    print("le vent ne varie pas"); ok = False
sys.exit(0 if ok else 1)
PYCHK
  then ok "aléa borné par ses amplitudes et effectivement variable"
  else ko "aléa : bornes ou variabilité"; fi

  # Polaire SYNTHÉTIQUE (aucune polaire réelle dans le dépôt) : à 90° et 10 nds,
  # la table donne 6 nds. Cap 90, vent vrai du 180 (TWA 90), sans courant.
  printf 'TWA\\TWS;0;10;20\n0;0;0;0\n90;0;6;10\n180;0;4;8\n' > "$TD/test.pol"
  printf 'enabled = 1\nhdg = 90\nstw = 2\nset = 0\ndrift = 0\ntwd = 180\ntws = 10\nstw_polar = 1\npolar = %s\n' "$TD/test.pol" > "$TD/p.ctl"
  ./n2k-sim --once --control "$TD/p.ctl" --state "$TD/p.state" > /dev/null 2>&1
  run_case "STW tirée de la polaire (90°, 10 nds → 6 nds)" grep -q '^stw = 6.00$' "$TD/p.state"
  # 135° et 15 nds : interpolation bilinéaire des quatre cellules 6, 10, 4, 8 → 7.
  printf 'enabled = 1\nhdg = 45\nset = 0\ndrift = 0\ntwd = 180\ntws = 15\nstw_polar = 1\npolar = %s\n' "$TD/test.pol" > "$TD/p.ctl"
  ./n2k-sim --once --control "$TD/p.ctl" --state "$TD/p.state" > /dev/null 2>&1
  run_case "STW interpolée (135°, 15 nds → 7 nds)" grep -q '^stw = 7.00$' "$TD/p.state"
  # Polaire illisible : repli sur la STW réglée, sans planter.
  printf 'enabled = 1\nhdg = 45\nstw = 5\nset = 0\ndrift = 0\ntwd = 180\ntws = 15\nstw_polar = 1\npolar = %s\n' "$TD/absente.pol" > "$TD/p.ctl"
  ./n2k-sim --once --control "$TD/p.ctl" --state "$TD/p.state" > /dev/null 2>&1
  run_case "polaire absente : repli sur la STW réglée" grep -q '^stw = 5.00$' "$TD/p.state"

  # « auto » repart de la dernière valeur réglée : au démarrage, le centre
  # vient du fichier (« auto 300 ») ; à chaud, recocher auto ne fait pas sauter.
  printf 'enabled = 1\nhdg = auto 45\ntwd = auto 300\n' > "$TD/au.ctl"
  ./n2k-sim --once --control "$TD/au.ctl" --state "$TD/au.state" > /dev/null 2>&1
  run_case "auto centré sur la valeur du fichier (vent)" grep -q '^twd = 300.0$' "$TD/au.state"
  run_case "auto centré sur la valeur du fichier (cap)"  grep -q '^hdg = 45.0$' "$TD/au.state"
  if python3 - "$TD" <<'PYLIVE'
import os, subprocess, sys, time, signal
td = sys.argv[1]; ctl = td + "/live.ctl"; st = td + "/live.state"
def write(t):
    open(ctl + ".tmp", "w").write(t); os.rename(ctl + ".tmp", ctl)
def twd():
    for l in open(st):
        if l.startswith("twd ="): return float(l.split("=")[1])
write("enabled = 1\ntwd = 300\n")
p = subprocess.Popen(["./n2k-sim", "--control", ctl, "--state", st],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
try:
    time.sleep(1.2)
    write("enabled = 1\ntwd = auto\n")          # auto recoché, sans centre
    time.sleep(1.5)
    v = twd()
finally:
    os.killpg(os.getpgid(p.pid), signal.SIGTERM)
sys.exit(0 if abs(v - 300) < 5 else 1)
PYLIVE
  then ok "auto recoché à chaud : pas de retour au 225"
  else ko "auto recoché à chaud : le vent a sauté"; fi

  # Trames N2K en parallèle du JSON (--actisense-out, port 2700 de la chaîne
  # simulée) : même état que le JSON. 6 nds de surface = 3,09 m/s → 309 = 0x0135,
  # soit les octets 35 01 du PGN 128259 ; le vent sort en trois trames 130306.
  printf 'enabled = 1\nhdg = 45\nstw = 6\nset = 90\ndrift = 2\ntwd = 105\ntws = 20\n' > "$TD/n.ctl"
  ./n2k-sim --once --control "$TD/n.ctl" --actisense-out "$TD/frames.txt" > /dev/null 2>&1
  run_case "trames N2K : STW identique au JSON" grep -qE ',128259,[0-9]+,255,8,ff,35,01,' "$TD/frames.txt"
  run_case "trames N2K : vent apparent + vrai, sans direction nord" sh -c "[ \$(grep -c ',130306,' '$TD/frames.txt') -eq 2 ]"
  # Lacet « non disponible » (ff 7f), pas 0 : un lacet à 0° passait pour un cap.
  run_case "trames N2K : lacet non disponible" grep -qE ',127257,[0-9]+,255,7,ff,ff,7f,' "$TD/frames.txt"
  # Cadence nominale des « Rapid Update » : 4 Hz. Le pas de boucle faisait
  # partir la position toutes les 300 ms (3,3 Hz) et le COG/SOG à 1 Hz.
  printf 'enabled = 1\nhdg = 0\nstw = 5\nset = 0\ndrift = 0\ntwd = 43\ntws = 4\n' > "$TD/c.ctl"
  ./n2k-sim --duration 3 --no-ais --control "$TD/c.ctl" --actisense-out "$TD/cad.txt" > /dev/null 2>&1
  np=$(grep -c ',129025,' "$TD/cad.txt"); nc=$(grep -c ',129026,' "$TD/cad.txt")
  if [ "$np" -ge 11 ] && [ "$np" -le 13 ] && [ "$nc" -ge 11 ] && [ "$nc" -le 13 ]; then
    ok "position et COG/SOG à 4 Hz ($np et $nc trames en 3 s)"
  else
    ko "cadence : $np positions et $nc COG/SOG en 3 s (attendu ~12)"
  fi
  run_case "trames N2K : courant (129291)" grep -q ',129291,' "$TD/frames.txt"

  # Interface : liste du dossier et refus des chemins détournés.
  if [ -x ./n2k-mux-web ] && command -v curl >/dev/null 2>&1; then
    printf 'TWS=5;0;1\n0;100;98\n' > "$TD/vagues.polwave.csv"
    printf 'pas une polaire\n' > "$TD/n_importe.csv"
    printf '[output]\ntalker = II\n' > "$TD/w.ini"
    ./n2k-mux-web "$TD/w.ini" --port 18124 --sim-control "$TD/w.ctl" --sim-state "$TD/w.state" --polar-dir "$TD" >/dev/null 2>&1 &
    WPID=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null "http://127.0.0.1:18124/" && break; done
    pol=$(curl -s "http://127.0.0.1:18124/api/polars")
    case "$pol" in *'"name":"test.pol","ok":true'*) ok "polaire proposée" ;; *) ko "polaire absente de la liste : $pol" ;; esac
    case "$pol" in *polwave*) ko "table de vagues proposée : $pol" ;; *) ok "table de vagues écartée" ;; esac
    case "$pol" in *'"name":"n_importe.csv","ok":false'*) ok "fichier illisible signalé" ;; *) ko "fichier illisible mal signalé : $pol" ;; esac
    rej=$(curl -s -X POST -H 'X-N2K-Mux: 1' --data-binary 'enabled = 1
polar = ../../../etc/passwd
' "http://127.0.0.1:18124/api/sim")
    case "$rej" in *'"ok":false'*) ok "chemin détourné refusé" ;; *) ko "chemin détourné accepté : $rej" ;; esac
    acc=$(curl -s -X POST -H 'X-N2K-Mux: 1' --data-binary 'enabled = 1
stw_polar = 1
polar = test.pol
' "http://127.0.0.1:18124/api/sim")
    case "$acc" in *'"ok":true'*) ok "polaire du dossier acceptée" ;; *) ko "polaire refusée : $acc" ;; esac
    run_case "chemin complet écrit" grep -q "^polar = $TD/test.pol$" "$TD/w.ctl"
    curl -s -X POST -H 'X-N2K-Mux: 1' --data-binary 'enabled = 1
twd = auto 300
' "http://127.0.0.1:18124/api/sim" > /dev/null
    run_case "interface : auto écrit avec son centre" grep -q '^twd = auto 300.00$' "$TD/w.ctl"
    ctr=$(curl -s "http://127.0.0.1:18124/api/sim")
    case "$ctr" in *'"twd":null,"twd_c":300.00'*) ok "interface : centre relu pour le curseur" ;; *) ko "centre non relu : $ctr" ;; esac
    curl -s -X POST -H 'X-N2K-Mux: 1' --data-binary 'enabled = 1
wind_random = 1
tws_var = 35
' "http://127.0.0.1:18124/api/sim" > /dev/null
    run_case "interface : amplitude de force bornée à 20 %" grep -q '^tws_var = 20.0$' "$TD/w.ctl"
    kill "$WPID" 2>/dev/null
  fi
  rm -rf "$TD"
fi

# ---- 5. interface web : aller-retour de l'API simulateur -------------------
say "== interface web (/api/sim) =="
if [ ! -x ./n2k-mux-web ]; then
  ko "n2k-mux-web absent (make d'abord)"
elif ! command -v curl >/dev/null 2>&1; then
  say "  (curl absent : contrôle sauté)"
else
  WCTL=$(mktemp); WINI=$(mktemp); PORT=18123
  printf '[output]\ntalker = II\n' > "$WINI"
  ./n2k-mux-web "$WINI" --port "$PORT" --sim-control "$WCTL" --sim-state "$WCTL.state" >/dev/null 2>&1 &
  WPID=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
  done
  curl -s -X POST -H 'X-N2K-Mux: 1' --data-binary 'enabled = 1
hdg = 200
stw = 7
set = auto
drift = auto
twd = 90
tws = 12
' "http://127.0.0.1:$PORT/api/sim" > /dev/null
  got=$(curl -s "http://127.0.0.1:$PORT/api/sim")
  kill "$WPID" 2>/dev/null
  case "$got" in
    *'"hdg":200.00'*) ok "POST puis GET /api/sim" ;;
    *) ko "aller-retour /api/sim : $got" ;;
  esac
  case "$got" in
    *'"set":null'*) ok "valeur « auto » conservée" ;;
    *) ko "valeur « auto » perdue : $got" ;;
  esac
  # Bascule de CHAÎNE : la commande ne part que si l'état demandé diffère de
  # l'état réel (fichier d'état frais = simulateur en marche), et arrêter le
  # simulateur ne le rend pas muet pour son prochain démarrage.
  WD=$(mktemp -d); WST="$WD/sim.state"
  ./n2k-mux-web "$WINI" --port "$PORT" --sim-control "$WD/sim.ctl" --sim-state "$WST" \
    --sim-start "touch $WD/started" --sim-stop "touch $WD/stopped" >/dev/null 2>&1 &
  WPID=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
  done
  simpost() { curl -s -X POST -H 'X-N2K-Mux: 1' --data-binary "enabled = $1
hdg = 10
" "http://127.0.0.1:$PORT/api/sim" > /dev/null; }
  simpost 1
  if [ -e "$WD/started" ]; then ok "bascule : démarre la chaîne simulée arrêtée"
  else ko "bascule : --sim-start non lancée"; fi
  touch "$WST"; rm -f "$WD/started"
  simpost 1
  if [ ! -e "$WD/started" ]; then ok "bascule : pas de relance à chaque réglage"
  else ko "bascule : --sim-start relancée alors que le simulateur tourne"; fi
  case "$(curl -s "http://127.0.0.1:$PORT/api/sim")" in
    *'"running":true'*) ok "bascule : état réel « en marche » (état frais)" ;;
    *) ko "bascule : running faux malgré un état frais" ;;
  esac
  simpost 0
  if [ -e "$WD/stopped" ] && grep -q '^enabled = 1' "$WD/sim.ctl"; then
    ok "bascule : retour au réseau réel, simulateur non muet"
  else ko "bascule : --sim-stop non lancée ou enabled = 0 écrit"; fi
  touch -d '-1 min' "$WST"
  case "$(curl -s "http://127.0.0.1:$PORT/api/sim")" in
    *'"running":false'*) ok "bascule : état périmé = simulateur arrêté" ;;
    *) ko "bascule : running vrai malgré un état périmé" ;;
  esac
  kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null
  rm -rf "$WD"
  # POST : en-tête anti-CSRF obligatoire, et un corps incomplet n'écrit RIEN.
  WD=$(mktemp -d)
  printf '[output]\ntalker = II\n\n[sources]\nSCX = 123\nVER = 456\n\n[priority]\n129026 = SCX, VER\n' > "$WD/c.ini"
  cp "$WD/c.ini" "$WD/ref.ini"
  ./n2k-mux-web "$WD/c.ini" --port "$PORT" --sim-control "$WD/sim.ctl" --sim-state "$WD/st" >/dev/null 2>&1 &
  WPID=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
  done
  code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary 'enabled = 0
' "http://127.0.0.1:$PORT/api/sim")
  if [ "$code" = "403" ] && [ ! -e "$WD/sim.ctl" ]; then ok "POST sans en-tête X-N2K-Mux refusé (403)"
  else ko "POST sans en-tête : code $code"; fi
  if python3 - "$PORT" <<'PYTRUNC'
import socket, sys, time
body = b"[output]\ntalker = II\n\n[sources]\nSCX = 123\nVER = 456\n\n[priority]\n129026 = SCX, VER\n"
part = body[:-6]                       # "129026 = SCX" : coupé avant ", VER"
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"POST /api/config HTTP/1.1\r\nHost: x\r\nX-N2K-Mux: 1\r\nContent-Length: %d\r\n\r\n" % len(body) + part)
time.sleep(2.5)                        # le reste n'arrive pas : délai de lecture dépassé
resp = s.recv(4096).decode(errors="replace")
sys.exit(0 if resp.startswith("HTTP/1.1 400") else 1)
PYTRUNC
  then
    if cmp -s "$WD/c.ini" "$WD/ref.ini"; then ok "corps POST incomplet : 400, config intacte"
    else ko "corps POST incomplet : config modifiée"; fi
  else
    ko "corps POST incomplet : pas de 400"
  fi
  kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null
  rm -rf "$WD"
  # Le JS de la page est écrit à la main dans une chaîne C : une coquille de
  # syntaxe casserait toute l'interface sans que rien ne le signale.
  if command -v node >/dev/null 2>&1; then
    ./n2k-mux-web "$WINI" --port "$PORT" --sim-control "$WCTL" --sim-state "$WCTL.state" >/dev/null 2>&1 &
    WPID=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
    done
    JS=$(mktemp --suffix=.js)
    curl -s "http://127.0.0.1:$PORT/" \
      | sed -n '/<script>/,/<\/script>/p' | sed '1d;$d' > "$JS"
    kill "$WPID" 2>/dev/null
    run_case "syntaxe du JS de la page" node --check "$JS"
    # Enregistrer les noms ne doit PAS effacer une source configurée absente du
    # tableau (appareil éteint, ou chaîne simulée) ni son nom dans les règles.
    MJS=$(mktemp --suffix=.js)
    { sed -n '/^function mergeSrcNames/,/^ return {sources:sources,rules:rules};}/p' "$JS"
      cat <<'NODECHK'
const ru={sources:[{name:'SCX',ident:'111'},{name:'DST_TB',ident:'222'}],
          rules:[{pgn:128267,disc:'',mode:'min',sources:['DST_BB','DST_TB']},
                 {pgn:129025,disc:'',mode:'priority',sources:['SCX','VER']}]};
// affichés : SCX renommé GPS ; un appareil neuf nommé DST_BB ; DST_TB (222) absent
const shown=new Map([['111','GPS'],['333','DST_BB'],['444','']]);
const r=mergeSrcNames(ru,shown);
const src=JSON.stringify(r.sources), rl=JSON.stringify(r.rules.map(x=>x.sources));
const want_src=JSON.stringify([{ident:'111',name:'GPS'},{ident:'222',name:'DST_TB'},{ident:'333',name:'DST_BB'}]);
const want_rl=JSON.stringify([['DST_BB','DST_TB'],['GPS','VER']]);
if(src!==want_src||rl!==want_rl){console.log('sources',src,'\nrègles',rl);process.exit(1);}
NODECHK
    } > "$MJS"
    run_case "noms : source absente du tableau conservée" node "$MJS"
    # esc() sert dans des attributs HTML : les guillemets doivent être échappés.
    { sed -n '/^function esc(/p' "$JS"
      echo "if(esc('a\"b<c>&d'+\"'\"+'e')!=='a&quot;b&lt;c&gt;&amp;d&#39;e')process.exit(1);"
      sed -n '/^const AISPGN=/p;/^function modesOf(/p' "$JS"
      echo "if(modesOf(129025).join()!=='priority'||modesOf(128267).join()!=='priority,min'||modesOf(129039).join()!=='priority,fusion')process.exit(1);"
    } > "$MJS"
    run_case "échappement HTML et modes proposés" node "$MJS"
    rm -f "$JS" "$MJS"
  else
    say "  (node absent : syntaxe JS non vérifiée)"
  fi
  rm -f "$WCTL" "$WINI"
fi

say ""
say "Total : $pass ok, $fail KO"
[ "$fail" -eq 0 ] || exit 1
