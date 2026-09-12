/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/* test_polar — lecture des variantes de polaires et interpolation bilinéaire.
 * Tables SYNTHÉTIQUES uniquement (valeurs inventées) : aucune polaire réelle
 * n'est embarquée dans le dépôt. */

#include "polar.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void chk(const char *name, bool cond)
{
    if (!cond) { printf("FAIL %s\n", name); fails++; }
}

static void near(const char *name, double got, double want)
{
    if (fabs(got - want) > 1e-6) {
        printf("FAIL %s : obtenu %.6f, attendu %.6f\n", name, got, want);
        fails++;
    }
}

int main(void)
{
    static polar_t p;

    /* --- 1. table simple « ; » : grille, milieux, repli, bornes --- */
    const char *simple =
        "TWA\\TWS;0;10;20\n"
        "0;0;0;0\n"
        "90;0;6;10\n"
        "180;0;4;8\n";
    chk("table simple lue", polar_parse_string(&p, simple));
    chk("3 colonnes de vent", p.n_tws == 3);
    chk("3 lignes d'angle", p.n_twa == 3);
    near("point de grille (90, 10)", polar_speed(&p, 90, 10), 6.0);
    near("point de grille (180, 20)", polar_speed(&p, 180, 20), 8.0);
    near("milieu en vent (90, 15)", polar_speed(&p, 90, 15), 8.0);
    near("milieu en angle (135, 10)", polar_speed(&p, 135, 10), 5.0);
    /* bilinéaire complet : (135,15) = moyenne de 6, 10, 4, 8 */
    near("milieu des deux (135, 15)", polar_speed(&p, 135, 15), 7.0);
    near("repli 270° = 90°", polar_speed(&p, 270, 10), 6.0);
    near("repli -90° = 90°", polar_speed(&p, -90, 10), 6.0);
    near("borné au-delà du vent max", polar_speed(&p, 90, 45), 10.0);
    near("borné sous le vent min", polar_speed(&p, 90, -3), 0.0);
    near("vitesse max relevée", p.max_speed, 10.0);

    /* --- 2. tabulations, CRLF, BOM, cellules vides en fin de ligne --- */
    const char *tabs =
        "\xEF\xBB\xBFTWA/TWS\t0\t10\t20\t\t\r\n"
        "0\t0\t0\t0\t\t\r\n"
        "90\t0\t6\t10\t\t\r\n";
    chk("tabulations + CRLF + BOM lues", polar_parse_string(&p, tabs));
    chk("cellules vides ignorées", p.n_tws == 3);
    near("valeur lue malgré CRLF", polar_speed(&p, 90, 20), 10.0);

    /* --- 3. décimale virgule avec séparateur « ; » --- */
    const char *comma =
        "TWA\\TWS;0;10\n"
        "0;0;0\n"
        "90;0;6,5\n";
    chk("décimale virgule lue", polar_parse_string(&p, comma));
    near("6,5 compris comme 6.5", polar_speed(&p, 90, 10), 6.5);

    /* --- 4. colonne de vent en double : pas de division par zéro --- */
    const char *dup =
        "twa/tws;0;10;10;20\n"
        "0;0;0;0;0\n"
        "90;0;6;6;10\n";
    chk("colonnes en double lues", polar_parse_string(&p, dup));
    double v = polar_speed(&p, 90, 10);
    chk("pas de NaN sur colonne en double", !isnan(v) && !isinf(v));
    near("valeur sur colonne en double", v, 6.0);
    near("au-delà de la colonne en double", polar_speed(&p, 90, 15), 8.0);

    /* --- 5. une seule ligne d'angle et une seule colonne --- */
    chk("table minimale lue", polar_parse_string(&p, "TWA\\TWS;12\n45;7\n"));
    near("table 1x1", polar_speed(&p, 120, 30), 7.0);

    /* --- 6. refus --- */
    chk("table de vagues refusée",
        !polar_parse_string(&p, "TWS=5;0;0.5;1\n0;100;98;94\n"));
    chk("angles décroissants refusés",
        !polar_parse_string(&p, "TWA\\TWS;0;10\n90;0;6\n45;0;4\n"));
    chk("en-tête sans vent refusé", !polar_parse_string(&p, "TWA\\TWS\n0\n"));
    chk("texte vide refusé", !polar_parse_string(&p, ""));

    /* --- 7. filtre de noms --- */
    chk("nom .pol accepté", polar_name_ok("CM50.pol"));
    chk("nom .CSV accepté", polar_name_ok("Oceanis 46.CSV"));
    chk("nom polwave refusé", !polar_name_ok("cruising.polwave.csv"));
    chk("nom .qvmg refusé", !polar_name_ok("CM50.qvmg"));
    chk("fichier caché refusé", !polar_name_ok(".x.pol"));

    printf("\n--- Récapitulatif ---\néchecs : %d\n", fails);
    return fails ? 1 : 0;
}
