# samples — captures de non-régression

Ces fichiers sont rejoués par `make test` (script `scripts/run-tests.sh`) :

- chaque `*.jsonl` doit être parsé INTÉGRALEMENT par `./test_jsonl` (sortie
  non nulle si une seule ligne échoue) ;
- `n2k-sim.jsonl` est un exemplaire de chaque PGN produit par `./n2k-sim --once`
  (couverture de tous les PGN que le mapper connaît, identités comprises).

## Ajouter une capture réelle

Le simulateur ne remplace pas un vrai bus. Pour élargir la couverture, déposer
ici la sortie de l'analyzer prise sur le matériel :

```sh
actisense-serial -r -s 230400 /dev/ttyNGX1 | analyzer -json -nv | head -5000 > samples/o3nav-ngx1.jsonl
# ou, en socketcan :
candump can0 | candump2analyzer | analyzer -json -nv | head -5000 > samples/o3nav-can0.jsonl
```

Rien d'autre à faire : `make test` prend tous les `samples/*.jsonl`. Une capture
réelle transforme une validation ponctuelle en garde-fou permanent, notamment
pour les PGN à champs variables (AIS, listes de satellites).

Attention à ne pas déposer de capture contenant des données que tu ne veux pas
publier (positions réelles, identités MMSI de tiers) : ce dossier est suivi par
git et le dépôt est public.
