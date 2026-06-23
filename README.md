# n2k-mux — mode d'emploi

`n2k-mux` lit le réseau **NMEA 2000** du bord, choisit **la meilleure source**
pour chaque donnée (position, cap, vent, profondeur, AIS…) et la redistribue à vos
logiciels de navigation, **en NMEA 2000** (pour qtVlm) **et en NMEA 0183** (pour
les tablettes et le legacy).

Concrètement, une fois installé, vous obtenez sur le réseau local trois points de
connexion :

| Vous voulez… | Connectez-vous à | Format |
|---|---|---|
| qtVlm en **NMEA 2000 natif** | `hôte:2700` (source NMEA **TCP**) | YDRAW |
| Tablettes / qtVlm en **NMEA 0183** | `hôte:10110` (TCP, + UDP) | 0183 |
| **Administrer** (config, équipements, charge) | `http://hôte:8080/` | Web |

> Ce mode d'emploi décrit l'installation type sur un PC de bord (Linux) relié au
> bus N2K par une passerelle **Actisense NGX-1/NGT-1**. Pour essayer sans matériel,
> sautez au [§7 Banc de test](#7-banc-de-test-sans-matériel).

---

## Sommaire

1. [Comment ça marche](#1-comment-ça-marche)
2. [Installation](#2-installation)
3. [Configurer votre bord](#3-configurer-votre-bord)
4. [Brancher qtVlm et les tablettes](#4-brancher-qtvlm-et-les-tablettes)
5. [Administrer par le web](#5-administrer-par-le-web)
6. [Dépannage](#6-dépannage)
7. [Banc de test sans matériel](#7-banc-de-test-sans-matériel)
8. [Annexes](#8-annexes) — options, PGN convertis, modes d'arbitrage, tests

---

## 1. Comment ça marche

```
Bus NMEA 2000
  │  passerelle Actisense NGX-1 en mode TRANSFER (N2K brut)
  ▼
actisense-serial ──┬─────────────────────────────► ydraw-bridge ─► TCP 2700  (N2K → qtVlm)
                   │
                   ▼
              analyzer -json -nv
                   │
                   ▼
        ┌──────── n2k-mux ─────────┐ arbitrage + conversion
        │                          │
        ▼                          ▼
   instruments 0183          n2k-mux --ais-json ─► n2kd (AIS → VDM)
        │                          │
        └──────────► kplex ◄───────┘ TCP 2599
                       │
                       ▼  TCP 10110 + UDP  (0183 → tablettes)
```

Les idées clés :

- **Identité stable.** Les équipements sont suivis par leur numéro de série
  (*Model Serial Code*), pas par leur adresse N2K. Vos priorités survivent donc à
  un changement d'adresse sur le bus.
- **Arbitrage par donnée.** Pour chaque type de mesure vous listez les sources
  préférées ; n2k-mux prend la première **vivante** (bascule automatique si elle se
  tait). Des modes spéciaux existent : `min` (profondeur, par sécurité), `max`
  (loch), `fusion` (AIS dédupliqué par MMSI).
- **Deux sorties simultanées.** Le N2K arbitré part en YDRAW pour qtVlm ; le même
  flux est converti en 0183 pour le reste.
- **Tout en C11, sans dépendance** hors libc (et `canboat` pour la passerelle/AIS).

---

## 2. Installation

### 2.1 Prérequis

- Linux, `gcc` (ou `clang`), `make`.
- [canboat](https://github.com/canboat/canboat) compilé : `actisense-serial`,
  `analyzer` et `n2kd`.
- `kplex` (paquet de la distribution) pour la sortie 0183.
- La passerelle **NGX-1/NGT-1 en mode Transfer** (N2K brut), **pas** Convert.

### 2.2 Compiler

```sh
git clone https://github.com/ozolli/n2k-mux && cd n2k-mux
make                 # daemon + UI web + ydraw-bridge + simulateur + testeurs
```

Vérifier que tout est sain (tous les testeurs à 0 échec) :

```sh
for t in test_config test_mapper test_arbiter test_nmea0183 test_aisdedup \
         test_sources test_stats test_netout test_ydraw; do ./$t && echo "$t OK"; done
./test_jsonl --selftest
```

### 2.3 Installer le service

```sh
sudo make install
```

Cela installe les binaires dans `/usr/local/bin`, deux services systemd
(`n2k-mux.service`, `n2k-mux-web.service`) et des fichiers d'exemple. Préparez
ensuite la configuration :

```sh
sudo cp /etc/n2k-mux/n2k-mux.ini.example /etc/n2k-mux/n2k-mux.ini
sudo cp /etc/default/n2k-mux.example     /etc/default/n2k-mux       # réglages
sudo cp kplex.conf.example               /etc/kplex.conf
$EDITOR /etc/n2k-mux/n2k-mux.ini   # voir §3
```

Dans **`/etc/default/n2k-mux`**, indiquez le port série, le baud et le chemin des
binaires canboat s'ils ne sont pas dans le `PATH` :

```sh
DEVICE=/dev/ttyNGX1
BAUD=230400
ACTISENSE=/home/vous/canboat/rel/linux-x86_64/actisense-serial
ANALYZER=/home/vous/canboat/rel/linux-x86_64/analyzer
N2KD=/home/vous/canboat/rel/linux-x86_64/n2kd
# YDRAW_PORT=2700   # port N2K/YDRAW (défaut 2700)
```

Désactivez un éventuel ancien service `kplex` autonome (il entrerait en conflit),
puis démarrez :

```sh
sudo systemctl disable --now kplex 2>/dev/null || true
sudo systemctl daemon-reload
sudo systemctl enable --now n2k-mux n2k-mux-web
```

`enable --now` démarre les services **et** les relance à chaque démarrage du PC.
En cas de défaillance d'un maillon (n2kd, kplex, analyzer…), systemd relance toute
la chaîne automatiquement (`Restart=always`).

Vérifier que ça tourne :

```sh
systemctl is-active n2k-mux n2k-mux-web      # → active / active
journalctl -u n2k-mux -f                     # suivre les logs
```

### 2.4 Variante adaptateur CAN (socketcan, ex. PEAK PCAN-USB)

Si o3nav est relié au bus par un **adaptateur socketcan** (PEAK PCAN-USB FD…) au
lieu de la passerelle série NGX-1, utilisez le service **`n2k-mux-can`** à la place
de `n2k-mux` (ne pas activer les deux). Prérequis : `sudo apt install can-utils`,
et indiquer le chemin de `candump2analyzer` dans `/etc/default/n2k-mux`
(`CANDUMP2ANALYZER=…`).

```sh
sudo systemctl disable --now n2k-mux 2>/dev/null || true
sudo systemctl enable --now n2k-mux-can n2k-mux-web
```

Le service monte `can0` (250 kbit/s) et `vcan0` au démarrage, puis sert **trois
sorties** : N2K local sur `vcan0`, N2K réseau en YDRAW/TCP **2700**, et NMEA 0183 sur
**10110** (kplex). Il agit en **filtre N2K→N2K** : arbitrage par identité, seules les
trames retenues sont réémises (`n2k-filter`), sans ré-encodage. La charge de bus
affichée devient **mesurée** (vraies trames) au lieu d'estimée. Réglages socketcan
(`CANIF`, `VCANIF`, `YDRAW_PORT`…) dans `/etc/default/n2k-mux` (voir l'exemple).

> Pour qtVlm en local sur o3nav : source **socketcan → `vcan0`**. À distance :
> source **TCP → `<hôte>:2700`**.

---

## 3. Configurer votre bord

La configuration est un fichier INI (`/etc/n2k-mux/n2k-mux.ini`). Modèle complet
commenté : `n2k-mux.ini.example`.

```ini
[output]
talker = II                 ; talker des phrases 0183 (qtVlm l'ignore)

[sources]
; nom logique = Model Serial Code (ou Unique Number) de l'équipement
SCX = 4830123               ; Furuno SCX-20 (cap/attitude/position)
VER = 917661                ; Veratron GO (GPS)
DH  = 000A520AF6A0          ; DataHub PredictWind (AIS, capteurs)

[priority]
; clé = pgn[/discriminant]   |   valeur = [mode:] liste de sources
129025          = SCX, VER            ; position : SCX d'abord, sinon VER
127250/Magnetic = SCX                 ; cap magnétique
128267          = min: DST_BB, DST_TB ; profondeur = la plus faible (sécurité)
128275          = max: DST_BB, DST_TB ; loch = le capteur le plus avancé
129039          = fusion: AIS, DH     ; AIS : em-trak prioritaire sur DataHub

[ignore]
src = 0                     ; ignorer l'adresse 0
pgn = 262161, 262656        ; messages de contrôle Actisense/CANboat

[rate]
; type de phrase = intervalle minimum en ms (limite le débit 0183)
GLL = 1000
GSV = 5000
```

**Les modes d'arbitrage :**

| Mode | Quand | Effet |
|---|---|---|
| `priority` (défaut) | cap, position, vent… | 1ʳᵉ source vivante, bascule auto |
| `min:` | profondeur | la valeur la plus faible (sécurité haut-fond) |
| `max:` | loch (distance dans l'eau) | la valeur la plus avancée (capteur hors d'eau sous-compte) |
| `fusion:` | AIS | toutes les sources fusionnées, dédup par MMSI |

Le `/discriminant` route selon un champ : `Reference` (Magnetic/True pour le cap,
Apparent/True pour le vent), `Temperature Source` (Sea/Outside).

### Trouver les *Model Serial Code* de vos équipements

Les identités ne circulent pas spontanément : le daemon les réclame au bus (ISO
Request), c'est automatique une fois le service lancé. Laissez tourner une minute,
puis ouvrez l'**interface web** (`http://hôte:8080/`, onglet **Sources**) : chaque
équipement vu y figure avec son fabricant, son modèle et son **serial**. Reportez
ce serial dans la section `[sources]`, donnez-lui un nom logique, puis enregistrez
(la config se recharge à chaud, voir §5).

> Après avoir nommé vos sources, **Enregistrer** dans le web suffit — pas besoin de
> redémarrer le service.

---

## 4. Brancher qtVlm et les tablettes

### qtVlm en NMEA 2000 (recommandé)

1. Dans qtVlm : **Configuration → NMEA → ajouter une connexion → TCP client**.
2. Adresse = l'IP du PC de bord, **port = 2700**.
3. Valider. qtVlm détecte automatiquement le format YDRAW et décode le N2K
   (position, cap, vent, satellites, **cibles AIS**…).

> Le N2K AIS et la constellation GPS nécessitent **qtVlm ≥ 5.12.27-beta2**.

### qtVlm ou tablettes en NMEA 0183

Connexion **TCP** sur **port 10110** (les instruments arbitrés + l'AIS en `!AIVDM`
y sont déjà fusionnés). Le 10110 est aussi diffusé en **UDP** sur le LAN.

### Accès depuis l'extérieur du bateau

Le 8080 (admin) et les ports de données ne doivent pas être exposés en clair sur
Internet. Passez par un tunnel SSH (`ssh -L 8080:127.0.0.1:8080 hôte`) ou un
pare-feu/redirection maîtrisé sur votre box.

---

## 5. Administrer par le web

Ouvrez `http://hôte:8080/`. Trois onglets :

- **Sources** — les équipements vus sur le bus, leur fabricant/modèle et les PGN
  qu'ils publient. C'est ici qu'on relève les serials (§3).
- **Charge** — charge estimée du bus N2K et du flux 0183, débit par PGN et par
  type de phrase (intervalle moyen en ms).
- **Configuration** — éditeur de l'INI avec **Valider** (vérifie la syntaxe) et
  **Enregistrer** (écrit le fichier *puis* recharge le daemon à chaud).

**Reload à chaud.** « Enregistrer » applique la nouvelle config **sans
redémarrage** : le fichier est validé, écrit, puis le daemon reçoit `SIGHUP` et
relit sa config (si le fichier est invalide, l'ancienne reste active et l'erreur
est journalisée). Le message de confirmation s'efface seul après 15 s.

> **Sécurité** : l'API web écrit la config et déclenche un rechargement. Le
> service écoute par défaut sur le LAN (`0.0.0.0:8080`) ; réservez-le à un réseau
> de confiance et ajoutez une authentification avant toute exposition plus large.

*Une ancienne GUI GTK3 (`make n2k-mux-gui`, `libgtk-3-dev`) existe encore, vouée à
être remplacée par cette interface web.*

---

## 6. Dépannage

| Symptôme | Piste |
|---|---|
| **Rien dans qtVlm sur 2700** | Source = TCP **client** (pas la section CAN/socketcan). Vérifier `systemctl is-active n2k-mux` et `ss -ltnp \| grep 2700`. |
| **Pas de cibles AIS en N2K** | Nécessite qtVlm **≥ 5.12.27-beta2**. En 0183 (10110) l'AIS passe quelle que soit la version. |
| **Aucune phrase 0183 sur 10110** | L'arbitrage n'a pas résolu les identités : vérifier que les serials de `[sources]` correspondent à l'onglet **Sources** du web. Le `--tx` (ISO Request) doit être actif (il l'est via le service). |
| **« Address already in use » au démarrage** | Un `n2kd` résiduel. Le service fait le ménage (`pkill -f "[n]2kd"`) ; sinon `sudo pkill -x n2kd` puis `systemctl restart n2k-mux`. |
| **Collision de port 2600** | `n2kd` réquisitionne 2597-2602. Le N2K/YDRAW est donc sur **2700** (réglable via `YDRAW_PORT`), surtout pas 2600. |
| **La config web ne s'enregistre pas** | Le fichier `/etc/n2k-mux/n2k-mux.ini` doit être inscriptible par l'utilisateur du service web (il tourne en root par défaut → OK). |
| **Le NGX-1 ne donne rien** | Il doit être en mode **Transfer** (N2K brut), pas Convert. Vérifier `DEVICE`/`BAUD` dans `/etc/default/n2k-mux`. |
| **Tout vérifier d'un coup** | `journalctl -u n2k-mux -u n2k-mux-web -f` |

Test de bout en bout sans toucher au bus (relit une capture) :

```sh
cat capture.raw | actisense-serial -r -s 230400 ... | analyzer -json | ./test_jsonl
```

---

## 7. Banc de test sans matériel

Le simulateur `n2k-sim` rejoue un flux N2K cohérent (bateau qui avance, cap qui
infléchit la route, cibles AIS) pour **tous les PGN compris**, sans bus ni
passerelle. La config compagnon `n2k-sim.ini` porte les identités simulées →
arbitrage résolu d'emblée.

```sh
./n2k-sim | ./n2k-mux n2k-sim.ini -v          # instruments → phrases 0183
./n2k-sim --once | ./n2k-mux n2k-sim.ini      # un de chaque PGN puis fin
./n2k-sim | ./n2k-mux --ais-json n2k-sim.ini  # AIS → dédup par MMSI
```

Options : `--once`, `--duration SEC`, `--no-ais`, `--tick MS`, `--actisense`.

**Chaîne 0183 complète sans matériel** — `./n2k-sim-run` monte
`n2k-sim → n2k-mux (+ --ais-json → n2kd) → kplex` et expose qtVlm sur **TCP 10110**.

**N2K vers qtVlm sans matériel** — le mode `--actisense` encode les PGN (AIS
compris) en trames N2K, servies en YDRAW par `ydraw-bridge` :

```sh
./n2k-sim --actisense | ./ydraw-bridge --port 2700   # qtVlm : TCP → hôte:2700
```

---

## 8. Annexes

### 8.1 Options de `n2k-mux` (pour un lancement manuel)

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
| `--sources CHEMIN` | publie les équipements vus en JSON (UI web) |
| `--stats CHEMIN` | publie débit/PGN + charge de bus estimée en JSON |
| `--no-0183` | désactive la sortie 0183 (arbitrage seul) |
| `--ais-json` | mode filtre AIS (JSON→JSON dédupliqué) devant `n2kd` |
| `-v` | journalise les décisions + un résumé sur stderr |

`n2k-mux-web` : `[config.ini] [--sources P] [--stats P] [--port N]
[--bind ADDR] [--reload-cmd CMD]` (défauts : port 8080, bind `0.0.0.0`).

### 8.2 Données converties (N2K → 0183)

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

### 8.3 Tests

Chaque module a son testeur autonome (`test_jsonl`, `test_registry`,
`test_nmea0183`, `test_config`, `test_arbiter`, `test_mapper`, `test_aisdedup`,
`test_sources`, `test_stats`, `test_netout`, `test_ydraw`). `./test_jsonl
--selftest` vérifie le typage des champs du parser sans entrée.

---

## Licence

Distribué sous licence **Apache 2.0** — voir [`LICENSE`](LICENSE).
© 2026 Olivier Zolli.

n2k-mux **n'inclut pas de code de canboat** : il consomme la sortie de
l'`analyzer` et délègue l'AIS à `n2kd` (process séparé). canboat est lui aussi
sous Apache 2.0.
