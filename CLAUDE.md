# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`n2k-mux` is a C11 NMEA 2000 multiplexer daemon, built up module by module. It is
designed to consume the JSON-lines output of canboat's `analyzer -json` (one JSON
object per line) and process NMEA 2000 messages.

Modules (a)–(f) are implemented (parser → registry → nmea0183 → config → arbiter
→ daemon); the GTK GUI (g) remains. The final binary `n2k-mux` runs the full
pipeline and is validated live on the bench. Each module ships its own test
harness (`test_*`). The Makefile and source comments are written incrementally
("livrés au fur et à mesure").

**Source comments and Makefile targets are written in French.** Match that
convention when editing existing files.

## Build & test

```sh
make            # builds the n2k-mux binary + all test harnesses
make n2k-mux    # build just the daemon (final binary)
make test_jsonl # build a single harness (test_registry, test_nmea0183, test_config, test_arbiter, test_mapper)
make clean      # remove build/, n2k-mux, and the test binaries
```

Objects land in `build/`; the linked binary `test_jsonl` is written to the repo
root. Override the compiler/flags via `make CC=clang` or `make CFLAGS=...`
(defaults: `-O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE`).

Run the parser test harness by piping JSONL into it (it reads stdin):

```sh
cat samples/xxx.raw | .../analyzer -json | ./test_jsonl            # one summary line per message
cat samples/xxx.raw | .../analyzer -json | ./test_jsonl --fields  # also dump every field + inferred type
```

It prints a recap to stderr (lines seen / parsed / failed) and **exits non-zero
if any line failed to parse** — usable as a regression check against real captures.

## Architecture

The parser (`src/jsonl.h`, `src/jsonl.c`) is deliberately **not** a general-purpose
JSON parser. Key design constraints to preserve when modifying it:

- **Zero per-line dynamic allocation.** The caller owns all storage: `jsonl_parse`
  fills a caller-provided `jsonl_msg_t`, and every string is a fixed-size buffer
  (`JSONL_*_LEN` / `JSONL_MAX_FIELDS` in the header). Do not introduce malloc per
  message.
- **Single left-to-right pass, no tree.** It extracts the known top-level keys
  (`src`, `pgn`, `prio`, `dst`, `timestamp`, `description`, `fields`, `version`)
  and flattens the nested `fields` object. `skip_value()` walks over anything
  unrecognized without failing the line.
- **Tolerant by design.** Unknown keys, missing fields, and malformed sub-values
  are skipped, not fatal. A line is considered useful (returns `true`) only if it
  has a `pgn` or is the canboat header line (`{"version":...}`, flagged
  `is_header`).
- **Field value typing.** Each field is typed as `JSONL_NUM`, `JSONL_STR`,
  `JSONL_NV` (the canboat `{"value":N,"name":"S"}` enum shape — both stored), or
  `JSONL_NULL`. Use the accessors `jsonl_get`, `jsonl_get_num`, `jsonl_get_str`
  rather than indexing `fields` directly; the `_num`/`_str` helpers return false
  on absent or type-incompatible fields and leave the out-param untouched.

Known limitations baked in intentionally: `\uXXXX` escapes are not decoded (the
4 hex digits are skipped and a `?` inserted — relevant canboat fields are ASCII).
Arbitrary arrays inside `fields` are still skipped (`JSONL_NULL`), **except** the
canboat repeating set emitted under the key `"list"` (e.g. the satellite list of
PGN 129540): it is captured into `jsonl_msg_t.list[]` (fixed cap `JSONL_MAX_LIST`
× `JSONL_LIST_FIELDS`, zero-alloc) and read via `jsonl_list_count` /
`jsonl_list_get_num` / `jsonl_list_get_str`.

When adding a new module, follow the existing Makefile pattern: add a
`$(BUILD)/<mod>.o` to the relevant target's prerequisites and, if it ships a test
harness, give it its own `<name>: ... build/<name>.o` rule like `test_jsonl`.

## Roadmap / modules à venir

Modules prévus (ordre d'implémentation) :
(a) jsonl     — parser JSON-lines [FAIT, validé sur bus réel : 615/615, 0 échec]
(b) registry  — table src→identité stable via PGN 60928 + 126996
                identité = Model Serial Code, cascade vers Unique Number
                [FAIT, validé sur banc réel : Veratron GO + DataHub PredictWind
                 résolus (serial), corrélation 60928+126996 par src OK]
                suit les devices par Unique Number à travers les changements
                d'adresse ; testeur : ./test_registry
                IMPORTANT : les identités ne circulent PAS en passif. Il faut
                émettre une ISO Request (PGN 59904) demandant 60928 puis 126996
                pour que les devices répondent. Le daemon (f) devra donc émettre
                (actisense-serial SANS -r, mode bidirectionnel) au démarrage puis
                périodiquement. Format trame TX : ,prio,pgn,src,dst,bytes,b0,b1,...
                ISO Request 60928 à tous : ,6,59904,0,255,3,00,ee,00
                ISO Request 126996 à tous : ,6,59904,0,255,3,14,f0,01
                (certains devices, ex. Veratron, ne répondent 60928 que sur
                 requête DIRIGÉE dst=<addr>, pas en broadcast).
(c) nmea0183  — générateur de phrases 0183 + checksum
                [FAIT, testeur ./test_nmea0183 : 0 échec, ancres + auto-cohérence]
                noyau (nmea_begin/field_*/end, checksum XOR) + constructeurs :
                HDG HDT HDM VTG GLL MWV DPT MTW ROT RSA VHW XDR(+attitude) MDA ZDA
                zéro alloc (nmea_t fourni) ; champ absent = NMEA_NA (NaN) → vide ;
                variation/déviation signées (Est +) ; talker par défaut "II".
                Reste à câbler par l'arbitre (e) selon le champ Reference, etc.
(d) config    — parser INI (sources nommées, priorités, cas spéciaux)
                [FAIT, testeur ./test_config : 0 échec]
                sections [output] talker, [sources] nom→identité,
                [priority] "pgn[/discriminant] = [mode:] liste", [ignore] src/pgn.
                modes : priority (défaut) | min (profondeur) | fusion (AIS).
                config_rule(pgn,disc) préfère la règle au discriminant le plus
                spécifique (préfixe) puis la générique. zéro alloc, tolérant.
(e) arbiter   — cœur : clé (pgn, src, discriminant) → source retenue
                [SÉLECTION FAITE, testeur ./test_arbiter : 0 échec]
                arbiter_decide(msg, now_ms) → ARB_ACCEPT / REJECT_* / IGNORED.
                priority = 1re source vivante (failover par timeout, déf. 5 s) ;
                min/fusion = toutes les sources listées vivantes participent.
                table pgn→champ discriminant codée en dur (130306/127250 →
                "Reference", 130312 → "Temperature Source"/"Source").
                exclut d'office src<=0 et PGN>=262144, + liste [ignore].
                2e passe (mapping) : module mapper (src/mapper.{h,c}).
                [FAIT, testeur ./test_mapper : 0 échec, + validé bout-en-bout
                 sur capture Veratron réelle : GLL/VTG/ZDA/GGA corrects]
                mapper_map(msg, decision, now) → 0..N phrases 0183. Couvre :
                129025→GLL, 129026→VTG, 126992→ZDA, 129029→GGA, 129539→GSA,
                129540→GSV (paginé 4 sats/phrase, via jsonl_msg_t.list[]),
                127250→HDG+HDM/HDT, 127251→ROT, 127257→XDR,
                130306→MWV(R)|MWV(T)+MWD,
                127245→RSA, 128259→VHW, 130312→MTW|MDA(air), 130314→MDA(press),
                128267→DPT (minimum des DST, état interne par source).
                AIS/VDM délégué à n2kd ; fusion/dédup MMSI = module aisdedup
                (mode n2k-mux --ais-json), testeur ./test_aisdedup : 0 échec.
(f) daemon    — main : stdin(analyzer) → arbiter → stdout(kplex)
                [FAIT, binaire ./n2k-mux ; validé LIVE en forme de prod sur le
                 banc : ISO Request émises → Veratron résolu → GLL/VTG/ZDA/GGA
                 produits en temps réel, checksums OK]
                pipeline : jsonl_parse → registry_observe → arbiter_decide →
                mapper_map → stdout. horloge CLOCK_MONOTONIC (now_ms).
                usage : n2k-mux [config.ini] [--tx FIFO] [--tx-interval SEC] [-v]
                --tx : émet les ISO Request (PGN 59904) sur un FIFO relié au
                stdin d'un actisense-serial bidirectionnel (SANS -r). FIFO ouvert
                en O_RDWR|O_NONBLOCK (évite l'interblocage de rendez-vous).
                config de référence : n2k-mux.ini.example.
                --sources CHEMIN : publie périodiquement les sources vues en JSON
                (module sources, défaut interval 5 s) pour la GUI.
(g) gui       — GTK3, édition de la config INI + liste des sources vues
                [FAIT, binaire ./n2k-mux-gui (make n2k-mux-gui) ; 0 warning ;
                 rendu validé (Xvfb + capture) : 2 onglets corrects]
                pont daemon→GUI : module sources (src/sources.{h,c}, testeur
                ./test_sources) → fichier JSON (défaut /run/n2k-mux/sources.json).
                GUI : onglet « Sources vues » (TreeView auto-rafraîchi, double-clic
                = copie l'identité) + onglet « Configuration » (éditeur INI texte
                brut → commentaires préservés ; « Valider » réutilise config_parse_
                string ; « Enregistrer » refuse si la config est invalide).
                usage : n2k-mux-gui [config.ini] [--sources CHEMIN] [--tab sources|config].
                make all NE construit PAS la GUI (garde le build OK sans GTK) ;
                make n2k-mux-gui la construit (nécessite libgtk-3-dev).

## Chaîne de production

NGX-1 en mode TRANSFER (N2K brut, 230400) — surtout PAS Convert
  → actisense-serial -s 230400 /dev/ttyNGX1 < tx.fifo   (bidirectionnel pour --tx)
  → analyzer -json -nv          (-nv requis par n2kd ; notre parser le gère)
  → tee ─┬→ n2k-mux conf --tx tx.fifo --sources sources.json ─┐ (instruments → stdin kplex)
         └→ n2k-mux --ais-json conf → n2kd  (AIS → TCP 2599) ─┘
  → kplex (mode=foreground : lit stdin + client tcp 2599) → TCP 10110 / UDP / log
  → qtVlm, tablettes

Tout est lancé par n2k-mux.service ; kplex est le DERNIER maillon du pipeline
(plus de service kplex autonome → Conflicts=kplex.service). Voir n2k-mux.service,
kplex.conf.example, n2k-mux.env.example. La GUI lit sources.json et édite conf.

CONTEXTE (cf. /home/ozolli/CR-NMEA-O3.pdf) : l'archi d'origine était NGX-1 en
mode Convert (N2K→0183 dans la passerelle) → kplex lisant /dev/ttyNGX1 en direct
(115200), priorité position SCX>Veratron gérée par l'ADRESSE N2K, sans arbitrage
logiciel. Le passage en Transfer + n2k-mux apporte : arbitrage par identité
stable, conversion de l'attitude (127257→XDR) et de la pression (130314→MDA) que
le NGX-1 Convert ne fait pas, et la dédup AIS.

Notes câblage AIS :
- n2kd lit le JSON sur stdin (EXIGE analyzer -json -nv), encode le VDM, sert le
  0183 sur TCP port+2 (défaut 2599 ; 2598 JSON ; 2601 AIS en JSON).
- --ais-json filtre en AMONT (en-tête + AIS de la source retenue par MMSI) → la
  sortie n2kd est 100% AIS. Avec une seule source AIS, n2kd direct suffit.

## Règles d'arbitrage

- Clé d'arbitrage : (pgn, src, discriminant)
  discriminant = "Reference" pour vent 130306 (Apparent/True) et cap 127250 (Magnetic/True)
                 "Temperature Source" pour 130312 (Sea→MTW eau / Outside→MDA air)
- Priorités : cap/attitude/position = SCX-20 > Veratron > MADBrain
              vent/safran = MADBrain
              profondeur = min(DST bâbord, DST tribord)  [sécurité]
              vitesse surface = MADBrain
              AIS = fusion em-trak + DataHub, dédup par MMSI
- Ignorer : src=0, PGN 262xxx (messages contrôle CANboat/Actisense)
- AIS/VDM : TRANCHÉ → délégation à canboat n2kd (encodeur VDM éprouvé). n2k-mux
  ne génère PAS de VDM. La dédup/fusion par MMSI se fait EN AMONT de n2kd via le
  mode `n2k-mux --ais-json` (JSON→JSON). Priorité em-trak (AIS) > DataHub (DH),
  encodée par l'ordre "fusion: AIS, DH". Validé live (631 !AIVDM + multi-fragments).

## Table de conversion PGN → NMEA 0183

Sources du bord (par nom logique, à mapper sur Model Serial Code dans l'INI) :
SCX = Furuno SCX-20 · MAD = MADBrain · DST_BB / DST_TB = DST810 bâbord/tribord
AIS = em-trak B953 · VER = Veratron GO · DH = DataHub PredictWind · M510 = IC-M510E

| PGN | Discriminant | Sources (ordre priorité) | Phrases 0183 |
|------|-------------|--------------------------|--------------|
| 129025 | — | SCX > VER > MAD | GLL |
| 129026 | — | SCX > VER > MAD | VTG |
| 129029 | — | SCX > VER | GGA, GNS, RMC, ZDA |
| 129539 | — | SCX > VER | GSA (mode fix + PDOP/HDOP/VDOP) |
| 129540 | — | SCX > VER | GSV (satellites en vue, paginé 4/phrase) |
| 126992 | — | SCX > VER > MAD | ZDA |
| 127250 | Reference (Magnetic/True) | SCX > MAD | HDG, HDT, HDM |
| 127251 | — | SCX > MAD | ROT |
| 127257 | — | SCX > MAD | XDR (pitch/roll) |
| 130306 | Reference (Apparent/True) | MAD | MWV(R) si Apparent ; MWV(T)+MWD si True |
| 127245 | — | MAD | RSA |
| 128267 | — | min(DST_BB, DST_TB) | DPT |
| 128259 | — | MAD | VHW |
| 130312 | Temperature Source = Sea | DST_BB > DST_TB | MTW |
| 130312 | Temperature Source = Outside | SCX | MDA (temp air) |
| 130314 | — | SCX | MDA (pression) |
| 129038/39/40/41, 129793/94, 129809/810 | — | fusion AIS + DH (dédup MMSI) | VDM |
| 129808 | — | M510 | DSC, DSE |

### Règles de génération
- **HDG** porte cap magnétique + déviation + variation (le plus complet) ; **HDT** = cap vrai ; **HDM** = cap magnétique seul. Générer selon le champ Reference du 127250.
- **MWV(R)** = vent apparent (Reference "R") ; **MWV(T)** = vent vrai (Reference "T") ; **MWD** = direction/vitesse vent vrai/sol. Le 130306 Apparent → MWV(R) ; le 130306 True → MWV(T) + MWD.
- **MDA** (Meteorological Composite) agrège pression (130314, champs 3-4 en bar) + température air (130312/Outside, champs 5-6). Une seule phrase MDA pour les deux.
- **MTW** = température eau, depuis 130312 dont Temperature Source = "Sea Temperature".
- **XDR** type pression/température/attitude. Pour 127257 : pitch + roll (pas le yaw).
- **DPT** : profondeur = valeur minimale des deux DST810 (sécurité haut-fond), pas de moyenne.

### Unités de la sortie `analyzer -json` (sans `-si`)
canboat applique `fixupUnit()` par défaut → unités « lisibles », PAS strictement SI :
- angles (rad) → **degrés** ; taux de giration (rad/s) → **deg/s** (ROT veut deg/min : ×60)
- **vitesses : m/s** (NON converties) → nœuds : ×1.943844 (1/0.514444)
- profondeur/distance : **mètres** · température (K) → **°C** · pression (Pa) → **bar**
- lat/lon : **degrés décimaux** · `Date` = "YYYY.MM.DD" · `Time` = "HH:MM:SS.ssss"
Le module mapper (e, 2e passe) concentre toutes ces conversions.

### Sentences explicitement NON générées
GRS (inutile, non géré par qtVlm), DBT/DBK/DBS (DPT seul suffit), VBW (qtVlm calcule
la dérive lui-même), 127252 Heave (pas d'usage), yaw du 127257, 129283/284 route
(qtVlm gère ses propres routes).

### Cible : qtVlm
qtVlm accepte (vérifié) : GGA GSA GSV RMC VTG GLL HDG HDT HDM RSA VHW VLW VWR VWT
MWV MWD MTW DBT DPT DBK DBS WPL RMB MDA XDR MMB PFEC ZDA VBW RPM RME ROT GNS GBS GST.
Validé par injection : MMB, XDR, MDA (pression + temp air lues correctement).

### Discriminants observés dans le flux analyzer réel
- 130306 : champ "Reference" = "Apparent" | "True (ground referenced to North)"
- 127250 : champ "Reference" = "Magnetic" | "True"
- 130312 : champ "Temperature Source" / "Source" = "Sea Temperature" | "Outside Temperature"
- Ignorer : src=0 et PGN 262xxx (262161 Actisense Operating mode, 262656 CANboat Startup)

## Module (g) GUI GTK — notes
- GTK3 (libgtk-3-dev). Cohérent avec Polar Doctor (même toolkit).
- La GUI réutilise le module `config` du daemon (pas de second parser INI).
- Elle lit /run/n2k-mux/sources.json (produit par le daemon) pour afficher
  les sources vues sur le bus, et édite le fichier INI de priorités.
- Logique (config, sources_dump) testable en CLI ; seule la couche GTK
  nécessite un affichage. Tester via écran local o3nav ou ssh -X.
- À implémenter en DERNIER, une fois le daemon fonctionnel.
- Validation visuelle : l'utilisateur lance et fournit des captures ;
  Claude Code ne voit pas le rendu directement.

## Développement de la GUI à distance (clients Wayland)
- `ssh -Y o3nav` puis `./n2k-mux-gui`. Sur un client Wayland (Ubuntu 24.04 par
  défaut), l'affichage passe par XWayland de façon transparente.
- Si la GUI tente de forcer Wayland à distance (échec, pas de Wayland via SSH),
  forcer le backend X11 : `GDK_BACKEND=x11 ./n2k-mux-gui`.
- Test du forwarding : `xeyes` doit s'afficher côté client.
- waypipe/VNC non nécessaires pour ce cas (dev ponctuel sur LAN).
