# n2k-mux

Multiplexeur **NMEA 2000 → NMEA 0183** pour le bord. Il consomme la sortie
JSON de l'`analyzer` de [canboat](https://github.com/canboat/canboat), choisit
**la meilleure source** pour chaque donnée (cap, position, vent, profondeur…)
selon des priorités configurables, et produit des phrases NMEA 0183 propres
(checksum correct) à destination de **kplex → qtVlm** et des tablettes.

Points clés :

- **Identité stable** des équipements (suivi par numéro de série / *Unique
  Number*, pas par adresse) → les priorités survivent aux changements d'adresse.
- **Arbitrage** par PGN : priorité (avec *failover* automatique), minimum
  (profondeur, par sécurité), fusion (AIS).
- **AIS** délégué à `n2kd` (encodeur VDM éprouvé), avec déduplication par MMSI
  en amont (em-trak prioritaire sur DataHub).
- **GUI GTK3** pour éditer la config et voir les équipements présents sur le bus.
- Écrit en C11, sans dépendance hors libc (GTK uniquement pour la GUI).

---

## 1. Construction

Prérequis : `gcc` (ou `clang`), `make`, et [canboat](https://github.com/canboat/canboat)
compilé (`actisense-serial`, `analyzer`, et `n2kd` pour l'AIS).

```sh
make            # le daemon n2k-mux + tous les testeurs
make n2k-mux    # uniquement le daemon
make n2k-mux-gui # la GUI (nécessite libgtk-3-dev)
make clean
```

Vérifier que tout passe :

```sh
make && for t in test_*; do ./$t < /dev/null >/dev/null && echo "$t OK"; done
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
129025          = SCX, VER, MAD     ; position : SCX d'abord, sinon VER, sinon MAD
127250/Magnetic = SCX, MAD          ; cap magnétique
128267          = min: DST_BB, DST_TB   ; profondeur = la plus faible (sécurité)
129039          = fusion: AIS, DH       ; AIS : em-trak prioritaire sur DataHub

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

Modes : `priority` (défaut, 1ʳᵉ source vivante), `min` (valeur minimale),
`fusion` (AIS, dédup MMSI). Le discriminant après `/` permet de router selon un
champ (ex. `Reference` = Magnetic/True pour le cap, Apparent/True pour le vent).

### Trouver les Model Serial Code

Les identités ne circulent pas spontanément : il faut interroger le bus. Le plus
simple est de laisser le daemon le faire (option `--tx`, voir §3) puis de lire
les équipements vus :

```sh
./test_registry < capture.jsonl          # ou via la GUI (onglet « Sources vues »)
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

### Exemple : instruments seuls (sans AIS)

```sh
actisense-serial -s 230400 /dev/ttyNGX1 < /run/n2k-mux/tx.fifo \
  | analyzer -json \
  | n2k-mux n2k-mux.ini --tx /run/n2k-mux/tx.fifo \
                        --sources /run/n2k-mux/sources.json \
  | kplex
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
  | kplex
```

`n2kd` sert l'AIS en `!AIVDM` sur **TCP 2599**. Côté kplex, ajouter une entrée
client TCP vers `127.0.0.1:2599` (filtrée sur `AIVDM,AIVDO` si besoin) en plus
de l'entrée recevant la sortie instruments de n2k-mux.

> Avec une seule source AIS, `--ais-json` est facultatif (`analyzer -nv | n2kd`
> suffit). Il sert à dédupliquer quand em-trak **et** DataHub sont présents.

### Options de `n2k-mux`

```
n2k-mux [config.ini] [--tx CHEMIN] [--tx-interval SEC]
                     [--sources CHEMIN] [--sources-interval SEC]
                     [--ais-json] [-v]
```

| Option | Rôle |
|---|---|
| `config.ini` | fichier de configuration (sinon : aucune règle) |
| `--tx CHEMIN` | émet les ISO Request sur ce FIFO (résolution d'identité) |
| `--tx-interval` | période d'émission (s, défaut 30) |
| `--sources CHEMIN` | publie les équipements vus en JSON (pour la GUI) |
| `--sources-interval` | période de publication (s, défaut 5) |
| `--stats CHEMIN` | publie le débit/PGN + la charge de bus estimée en JSON |
| `--stats-interval` | période de publication des stats (s, défaut 5) |
| `--ais-json` | mode filtre AIS (JSON→JSON dédupliqué) devant `n2kd` |
| `-v` | journalise les décisions + un résumé stats sur stderr |

### Charge du bus NMEA 2000

`--stats CHEMIN` écrit périodiquement un JSON avec le **débit par PGN** (fréquence
en Hz + total) et une **charge de bus estimée** (`bus_load_pct`). n2k-mux étant en
aval de l'`analyzer` (il voit des messages, pas les trames CAN), la charge est
*estimée* : nombre de trames par message déduit d'une table fast-packet, charge
≈ trames/s × 130 bits ÷ 250 kbit/s (précision ~±15 %). Avec `-v`, un résumé
s'affiche aussi sur stderr (journal systemd).

### Test manuel (avant installation)

Pour valider la chaîne en réel sans rien installer (FIFO + log dans `/tmp`, pas
de sudo si tu es dans le groupe `dialout`) :

```sh
scripts/n2k-mux-test.sh [config.ini]      # défaut : /tmp/n2kmux.ini
```

Le script monte tout le pipeline (NGX-1 en **Transfer** requis), expose qtVlm sur
**TCP 127.0.0.1:10110**, journalise dans `/tmp/n2k-mux-test.log` et publie les
sources vues dans `/tmp/n2k-mux-test-sources.json`. `Ctrl-C` arrête et nettoie.

### Lancement automatique (systemd)

Le daemon tourne en service via `n2k-mux.service` (toute la chaîne :
`actisense → analyzer -nv → tee → n2k-mux + n2kd → kplex`).

```sh
# 1. installer binaires + service + exemples (les binaires canboat doivent
#    aussi être installés, ou pointés via /etc/default/n2k-mux)
sudo make install                 # ajouter GUI=1 pour installer aussi la GUI

# 2. configurer
sudo cp /etc/n2k-mux/n2k-mux.ini.example /etc/n2k-mux/n2k-mux.ini
sudo cp /etc/default/n2k-mux.example /etc/default/n2k-mux   # facultatif (réglages)
$EDITOR /etc/n2k-mux/n2k-mux.ini /etc/default/n2k-mux

# 3. activer
sudo systemctl daemon-reload
sudo systemctl enable --now n2k-mux
journalctl -u n2k-mux -f
```

Réglages (port, baud, chemins des binaires) dans **`/etc/default/n2k-mux`**
(voir `n2k-mux.env.example`). Le service crée `/run/n2k-mux/` (FIFO d'émission
+ `sources.json`) et **émet les ISO Request au démarrage puis périodiquement**
(`--tx-interval`, défaut 30 s).

**Prérequis NGX-1 : mode Transfer.** n2k-mux a besoin du N2K *brut* ; la
passerelle Actisense doit être en mode **Transfer** (et non Convert), via
Actisense Toolkit. En mode Convert, ne pas utiliser n2k-mux (kplex lit alors
directement le 0183 du NGX-1).

**kplex est intégré au pipeline** (dernier maillon de `n2k-mux.service`). Il faut
donc désactiver l'ancien service kplex autonome et adapter sa config :

```sh
sudo systemctl disable --now kplex          # kplex est désormais lancé par n2k-mux
sudo cp /etc/kplex.conf /etc/kplex.conf.bak # sauvegarde
sudo cp kplex.conf.example /etc/kplex.conf  # voir kplex.conf.example
```

La config fournie (`kplex.conf.example`) met kplex en **`mode=foreground`**, lit
les instruments sur **stdin** (`[file] filename=-`) et l'AIS de `n2kd` en
**client TCP `127.0.0.1:2599`**, en conservant les sorties (TCP 10110, UDP, log).

---

## 4. Interface graphique

```sh
./n2k-mux-gui n2k-mux.ini --sources /run/n2k-mux/sources.json
```

- **Sources vues** : équipements présents sur le bus (adresse, identité,
  fabricant, modèle, n° série, nombre de messages, **PGNs publiés**).
  Auto-rafraîchi. **Double-clic** sur une ligne = copie l'identité (à coller
  dans `[sources]`). La colonne « PGNs publiés » liste les PGN émis par chaque
  appareil — pratique pour savoir qui parle quoi et régler `[priority]`.
- **Configuration** : éditeur du fichier INI. *Valider* vérifie la syntaxe,
  *Enregistrer* écrit le fichier (refuse une config invalide). *Enregistrer et
  redémarrer* écrit puis relance le service via `pkexec` (popup d'authentification)
  pour appliquer immédiatement les changements — sinon le daemon ne relit sa
  config qu'au prochain redémarrage.

À distance : `ssh -Y o3nav` puis lancer la GUI (forcer `GDK_BACKEND=x11` si le
client est sous Wayland).

---

## 5. Données converties

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
| 128259 | Vitesse surface | VHW |
| 128267 | Profondeur | DPT (min des sondeurs) |
| 130312 | Température | MTW (eau) / MDA (air) |
| 130314 | Pression | MDA |
| 129038/39/40/41, 129793/94, 129809/810 | AIS | !AIVDM (via n2kd) |

---

## 6. Tests

Chaque module a son testeur autonome (`test_jsonl`, `test_registry`,
`test_nmea0183`, `test_config`, `test_arbiter`, `test_mapper`, `test_aisdedup`,
`test_sources`). Le testeur du parser sert aussi de contrôle de non-régression
sur une vraie capture :

```sh
cat capture.raw | actisense-serial -r -s 230400 ... | analyzer -json | ./test_jsonl
```

---

## Licence

Distribué sous licence **Apache 2.0** — voir [`LICENSE`](LICENSE).
© 2026 Olivier Zolli.

n2k-mux **n'inclut pas de code de canboat** : il consomme la sortie de
l'`analyzer` et délègue l'AIS à `n2kd` (lancé comme process séparé). canboat est
lui aussi sous Apache 2.0.
