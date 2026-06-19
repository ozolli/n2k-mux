# n2k-mux

Multiplexeur **NMEA 2000 → NMEA 0183** pour le bord. Il consomme la sortie
JSON de l'`analyzer` de [canboat](https://github.com/canboat/canboat), choisit
**la meilleure source** pour chaque donnée (cap, position, vent, profondeur…)
selon des priorités configurables, et produit des phrases NMEA 0183 propres
(checksum correct) à destination de **kplex → qtVlm** et des tablettes.

Points clés :

- **Identité stable** des équipements (suivi par numéro de série / *Unique
  Number*, pas par adresse) → les priorités survivent aux changements d'adresse.
- **Arbitrage** par PGN : `priority` (avec *failover* automatique), `min`
  (profondeur, par sécurité), `max` (loch : le capteur sorti de l'eau
  sous-compte), `fusion` (AIS, dédup par MMSI).
- **AIS** délégué à `n2kd` (encodeur VDM éprouvé), avec déduplication par MMSI
  en amont (em-trak prioritaire sur DataHub).
- **Interface de gestion web** (zéro dépendance) pour éditer la config et voir
  les équipements + la charge du bus — consultable depuis tablette/téléphone.
  Une GUI GTK3 historique reste disponible.
- **Reload de la config à chaud** (`SIGHUP`) : pas de redémarrage pour appliquer.
- **Banc de test sans matériel** : le simulateur `n2k-sim` rejoue tous les PGN
  compris, en JSON (0183) **ou** en trames N2K binaires (sortie YDRAW vers qtVlm).
- Écrit en **C11, sans dépendance hors libc** (GTK uniquement pour l'ancienne GUI).

---

## 1. Construction

Prérequis : `gcc` (ou `clang`), `make`, et [canboat](https://github.com/canboat/canboat)
compilé (`actisense-serial`, `analyzer`, et `n2kd` pour l'AIS).

```sh
make             # le daemon, l'UI web, le simulateur, ydraw-bridge + tous les testeurs
make n2k-mux     # uniquement le daemon
make n2k-mux-web # uniquement l'interface web
make n2k-sim     # uniquement le simulateur (banc sans matériel)
make n2k-mux-gui # l'ancienne GUI (nécessite libgtk-3-dev) — NON construite par `make`
make clean
```

Vérifier la non-régression (tous les testeurs à 0 échec) :

```sh
make
for t in test_config test_mapper test_arbiter test_nmea0183 test_aisdedup \
         test_sources test_stats test_netout test_ydraw; do ./$t && echo "$t OK"; done
./test_jsonl --selftest      # auto-test du typage des champs du parser
```

---

## 2. Configuration

La config est un fichier INI (voir `n2k-mux.ini.example` pour un modèle complet).

```ini
[output]
talker = II                 ; talker des phrases 0183 (qtVlm l'ignore)

[sources]
; nom logique = Model Serial Code (ou Unique Number) de l'équipement
SCX = 4830123               ; Furuno SCX-20
VER = 917661                ; Veratron GO GPS
DH  = 000A520AF6A0          ; DataHub PredictWind

[priority]
; clé = pgn[/discriminant]   |   valeur = [mode:] liste de sources
129025          = SCX, VER, MAD       ; position : SCX d'abord, sinon VER, sinon MAD
127250/Magnetic = SCX, MAD            ; cap magnétique
128267          = min: DST_BB, DST_TB ; profondeur = la plus faible (sécurité)
128275          = max: DST_BB, DST_TB ; loch = le capteur le plus avancé
129039          = fusion: AIS, DH     ; AIS : em-trak prioritaire sur DataHub

[ignore]
src = 0
pgn = 262161, 262656        ; messages de contrôle Actisense/CANboat

[rate]
; type de phrase = intervalle minimum en ms (0/absent = pas de limite)
GLL = 1000                  ; au plus 1 position/s
GSV = 5000                  ; satellites toutes les 5 s
```

La section `[rate]` limite le débit de la **sortie 0183** par type de phrase
(utile pour les liaisons lentes ou les PGN bavards). Une rafale multi-phrases
(ex. les pages `GSV`) passe toujours en entier d'un coup.

Modes : `priority` (défaut, 1ʳᵉ source vivante avec *failover*), `min` (valeur
minimale, profondeur), `max` (valeur maximale, loch), `fusion` (AIS, dédup MMSI).
Le discriminant après `/` permet de router selon un champ (ex. `Reference` =
Magnetic/True pour le cap, Apparent/True pour le vent).

### Trouver les Model Serial Code

Les identités ne circulent pas spontanément : il faut interroger le bus. Le plus
simple est de laisser le daemon le faire (option `--tx`, voir §3) puis de lire
les équipements vus dans l'**interface web** (onglet « Sources ») ou en CLI :

```sh
./test_registry < capture.jsonl
```

Reporter le `serial`/`unique` affiché dans la section `[sources]`.

---

## 3. Mise en service

### Chaîne complète

```
NGX-1 / NGT-1 (USB, 230400 baud)
  → actisense-serial → analyzer -json -nv
  → tee ─┬→ n2k-mux            (instruments arbitrés → 0183)
         └→ n2k-mux --ais-json → n2kd   (AIS → !AIVDM)
  → kplex (agrège tout) → qtVlm, tablettes, polar_doctor
```

### Émission des requêtes d'identité (`--tx`)

Pour que les équipements déclarent leur identité (PGN 60928/126996), le daemon
émet des *ISO Request* (PGN 59904). Cela suppose un `actisense-serial` en mode
**bidirectionnel** (sans `-r`) dont l'entrée est un FIFO alimenté par n2k-mux :

```sh
mkfifo /run/n2k-mux/tx.fifo
```

### Exemple : avec l'AIS

`analyzer` doit tourner en `-json -nv` (requis par `n2kd`), et l'AIS part dans
une branche parallèle vers `n2kd` :

```sh
actisense-serial -s 230400 /dev/ttyNGX1 < /run/n2k-mux/tx.fifo \
  | analyzer -json -nv \
  | tee >(n2k-mux --ais-json n2k-mux.ini | n2kd) \
  | n2k-mux n2k-mux.ini --tx /run/n2k-mux/tx.fifo \
                        --sources /run/n2k-mux/sources.json \
                        --stats   /run/n2k-mux/stats.json \
  | kplex
```

`n2kd` sert l'AIS en `!AIVDM` sur **TCP 2599** ; côté kplex, une entrée client
TCP `127.0.0.1:2599` s'ajoute à l'entrée recevant les instruments de n2k-mux.

> Avec une seule source AIS, `--ais-json` est facultatif (`analyzer -nv | n2kd`
> suffit). Il sert à dédupliquer quand em-trak **et** DataHub sont présents.

### Options de `n2k-mux`

```
n2k-mux [config.ini] [--tx CHEMIN] [--tx-interval SEC]
                     [--sources CHEMIN] [--sources-interval SEC]
                     [--stats CHEMIN] [--stats-interval SEC]
                     [--no-0183] [--ais-json] [-v]
```

| Option | Rôle |
|---|---|
| `config.ini` | fichier de configuration (sinon : aucune règle) |
| `--tx CHEMIN` | émet les ISO Request sur ce FIFO (résolution d'identité) |
| `--tx-interval` | période d'émission (s, défaut 30) |
| `--sources CHEMIN` | publie les équipements vus en JSON (UI web / GUI) |
| `--sources-interval` | période de publication (s, défaut 5) |
| `--stats CHEMIN` | publie le débit/PGN + la charge de bus estimée en JSON |
| `--stats-interval` | période de publication des stats (s, défaut 5) |
| `--no-0183` | désactive la génération 0183 (arbitrage seul) |
| `--ais-json` | mode filtre AIS (JSON→JSON dédupliqué) devant `n2kd` |
| `-v` | journalise les décisions + un résumé stats sur stderr |

**Reload à chaud (`SIGHUP`)** : le daemon relit son fichier de config sur
`SIGHUP`, sans redémarrage (modes principal **et** `--ais-json`). Si le nouveau
fichier est invalide, l'ancienne config reste active et l'erreur est journalisée.
C'est ce que l'interface web utilise pour « Enregistrer ».

`--no-0183` coupe toute sortie 0183 tout en gardant l'arbitrage
(registre/sources/stats) : prélude au flux **NMEA 2000 arbitré** (qtVlm sait lire
le N2K nativement, sur le bus CAN ou sur le réseau — voir §6).

### Charge du bus NMEA 2000

`--stats CHEMIN` écrit périodiquement un JSON avec le **débit par PGN** et une
**charge de bus estimée** (`bus_load_pct`). n2k-mux étant en aval de l'`analyzer`
(il voit des messages, pas les trames CAN), la charge est *estimée* : trames par
message déduites d'une table fast-packet, charge ≈ trames/s × 130 bits ÷
250 kbit/s (~±15 %). Avec `-v`, un résumé s'affiche aussi sur stderr.

### Lancement automatique (systemd)

Le daemon tourne en service via `n2k-mux.service` (toute la chaîne :
`actisense → analyzer -nv → tee → n2k-mux + n2kd → kplex`, orchestrée par le
script `n2k-mux-run`).

```sh
sudo make install                 # ajouter GUI=1 pour installer aussi la GUI GTK
sudo cp /etc/n2k-mux/n2k-mux.ini.example /etc/n2k-mux/n2k-mux.ini
sudo cp /etc/default/n2k-mux.example /etc/default/n2k-mux   # facultatif (réglages)
$EDITOR /etc/n2k-mux/n2k-mux.ini /etc/default/n2k-mux
sudo systemctl daemon-reload
sudo systemctl enable --now n2k-mux
journalctl -u n2k-mux -f
```

Réglages (port, baud, chemins des binaires) dans **`/etc/default/n2k-mux`**
(voir `n2k-mux.env.example`). Le service crée `/run/n2k-mux/` (FIFO + `sources.json`
+ `stats.json`) et émet les ISO Request au démarrage puis périodiquement.

**Prérequis NGX-1 : mode Transfer.** n2k-mux a besoin du N2K *brut* ; la
passerelle Actisense doit être en mode **Transfer** (et non Convert). En mode
Convert, ne pas utiliser n2k-mux (kplex lit alors directement le 0183 du NGX-1).

**kplex est intégré au pipeline** (dernier maillon de `n2k-mux.service`).
Désactiver l'ancien service kplex autonome et utiliser la config fournie :

```sh
sudo systemctl disable --now kplex
sudo cp kplex.conf.example /etc/kplex.conf
```

`kplex.conf.example` met kplex en **`mode=foreground`**, lit les instruments sur
**stdin** (`[file] filename=-`) et l'AIS de `n2kd` en **client TCP `127.0.0.1:2599`**,
en conservant les sorties (TCP 10110, UDP, log).

---

## 4. Interface de gestion web (`n2k-mux-web`)

Serveur HTTP minimal, **zéro dépendance** (réutilise le module config du daemon).
Trois onglets : **Sources** (équipements vus + PGN publiés), **Charge** (charge
bus N2K estimée + flux 0183, intervalle moyen par PGN/phrase en ms), et
**Configuration** (éditeur de l'INI avec validation).

```sh
n2k-mux-web /etc/n2k-mux/n2k-mux.ini \
  --sources /run/n2k-mux/sources.json --stats /run/n2k-mux/stats.json \
  --reload-cmd "pkill -HUP -x n2k-mux"
# → http://127.0.0.1:8080/
```

| Option | Rôle |
|---|---|
| `config.ini` | fichier INI édité par l'interface |
| `--sources` / `--stats` | JSON publiés par le daemon (onglets Sources / Charge) |
| `--port N` | port d'écoute (défaut 8080) |
| `--bind ADDR` | adresse d'écoute (défaut `127.0.0.1` ; `0.0.0.0` = LAN) |
| `--reload-cmd CMD` | commande lancée après une sauvegarde réussie |

**Sauvegarde sans privilège** : « Enregistrer » valide la config, écrit le
fichier, puis exécute `--reload-cmd` (ex. `pkill -HUP -x n2k-mux`) → le daemon
relit à chaud par `SIGHUP`. Plus besoin de `pkexec` ni de `systemctl restart`.

> **Sécurité** : l'API écrit la config et déclenche une commande. Garder le bind
> sur `127.0.0.1` (accès distant via tunnel SSH `ssh -L 8080:127.0.0.1:8080 hôte`),
> ou réserver `--bind 0.0.0.0` à un LAN de confiance (et prévoir une
> authentification avant toute exposition au-delà).

### Ancienne GUI GTK3 (`n2k-mux-gui`)

Toujours disponible (`make n2k-mux-gui`, nécessite `libgtk-3-dev`), mêmes onglets.
Vouée à être remplacée par l'interface web. À distance : `ssh -Y hôte` puis lancer
la GUI (forcer `GDK_BACKEND=x11` si le client est sous Wayland).

---

## 5. Banc de test sans matériel (`n2k-sim`)

Le simulateur `n2k-sim` génère un flux NMEA 2000 cohérent (position intégrée le
long du COG, cap ≈ COG, etc.) pour **tous les PGN compris** + l'identité, sans
bus ni passerelle. La config compagnon `n2k-sim.ini` porte les identités émises
→ arbitrage résolu d'emblée.

```sh
./n2k-sim | ./n2k-mux n2k-sim.ini -v          # instruments → phrases 0183
./n2k-sim --once | ./n2k-mux n2k-sim.ini      # un de chaque PGN puis fin
./n2k-sim | ./n2k-mux --ais-json n2k-sim.ini  # AIS → dédup par MMSI
```

Options : `--once`, `--duration SEC`, `--no-ais`, `--tick MS`, `--actisense`.
Chaîne 0183 complète sans matériel : **`./n2k-sim-run`** monte
`n2k-sim → n2k-mux (+ --ais-json → n2kd) → kplex` et expose qtVlm sur **TCP 10110**.

---

## 6. Sortie NMEA 2000 (YDRAW) — expérimental

qtVlm lit le NMEA 2000 nativement, soit sur le bus CAN, soit **sur le réseau** au
format **YDRAW** (Yacht Devices RAW text, auto-détecté sur une source NMEA
TCP/UDP). Deux briques sont en place pour ce flux :

- **`ydraw`** (`src/ydraw.{h,c}`) : formateur YDRAW + (dé)codage de l'ID CAN
  29 bits, avec re-fragmentation fast-packet.
- **`netout`** (`src/netout.{h,c}`) : serveur TCP de diffusion (fan-out vers N
  clients), zéro alloc, sockets non bloquants.

Outil de test **`ydraw-bridge`** : lit le format actisense sur stdin et sert en
YDRAW/TCP (single-frame **et** fast-packet re-fragmenté). Couplé au simulateur en
mode `--actisense`, il valide la réception N2K de qtVlm sans matériel :

```sh
./n2k-sim --actisense | ./ydraw-bridge --port 2600   # qtVlm : source NMEA TCP → hôte:2600
```

> Le mode `--actisense` du simulateur encode aussi l'AIS (129038/039/809/810) en
> trames N2K, re-fragmentées par `ydraw-bridge`. Les échelles et offsets binaires
> sont validés par aller-retour dans `analyzer -format YDWG02 -json` / `-format FAST`.

Le flux N2K arbitré du daemon lui-même (lecture du bus → arbitrage → YDRAW →
`netout`) reste à câbler ; `kplex` demeure l'endpoint NMEA 0183.

---

## 7. Données converties

| PGN | Donnée | Phrases 0183 |
|---|---|---|
| 129025 | Position | GLL |
| 129026 | COG/SOG | VTG |
| 129029 | Position GNSS | GGA |
| 129539 | DOP / mode de fix | GSA |
| 129540 | Satellites en vue | GSV (paginé) |
| 126992 | Heure système | ZDA |
| 127250 | Cap | HDG + HDM (mag) / HDT (vrai) |
| 127251 | Taux de giration | ROT |
| 127257 | Attitude | XDR (pitch/roll) |
| 130306 | Vent | MWV(R) / MWV(T) + MWD |
| 127245 | Barre | RSA |
| 129291 | Courant (set/drift) | VDR |
| 128259 | Vitesse surface | VHW |
| 128267 | Profondeur | DPT (min des sondeurs) |
| 128275 | Distance dans l'eau (loch) | VLW (max des sondeurs) |
| 130316 | Température | MTW (eau) / MDA (air) — 130312 déprécié, accepté en entrée |
| 130314 | Pression | MDA |
| 129038/39/40/41, 129793/94/95/96/97/98, 129801/02, 129809/810 | AIS | !AIVDM (via n2kd) |

---

## 8. Tests

Chaque module a son testeur autonome (`test_jsonl`, `test_registry`,
`test_nmea0183`, `test_config`, `test_arbiter`, `test_mapper`, `test_aisdedup`,
`test_sources`, `test_stats`, `test_netout`, `test_ydraw`). Le testeur du parser
sert aussi de contrôle de non-régression sur une vraie capture (sortie non nulle
si une ligne échoue) :

```sh
cat capture.raw | actisense-serial -r -s 230400 ... | analyzer -json | ./test_jsonl
./test_jsonl --selftest      # auto-test du typage des champs (sans entrée)
```

---

## Licence

Distribué sous licence **Apache 2.0** — voir [`LICENSE`](LICENSE).
© 2026 Olivier Zolli.

n2k-mux **n'inclut pas de code de canboat** : il consomme la sortie de
l'`analyzer` et délègue l'AIS à `n2kd` (lancé comme process séparé). canboat est
lui aussi sous Apache 2.0.
