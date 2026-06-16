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
```

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
| `--ais-json` | mode filtre AIS (JSON→JSON dédupliqué) devant `n2kd` |
| `-v` | journalise les décisions sur stderr |

---

## 4. Interface graphique

```sh
./n2k-mux-gui n2k-mux.ini --sources /run/n2k-mux/sources.json
```

- **Sources vues** : équipements présents sur le bus (adresse, identité,
  fabricant, modèle, n° série, nombre de messages). Auto-rafraîchi.
  **Double-clic** sur une ligne = copie l'identité (à coller dans `[sources]`).
- **Configuration** : éditeur du fichier INI. *Valider* vérifie la syntaxe,
  *Enregistrer* refuse une config invalide.

À distance : `ssh -Y o3nav` puis lancer la GUI (forcer `GDK_BACKEND=x11` si le
client est sous Wayland).

---

## 5. Données converties

| PGN | Donnée | Phrases 0183 |
|---|---|---|
| 129025 | Position | GLL |
| 129026 | COG/SOG | VTG |
| 129029 | Position GNSS | GGA |
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
