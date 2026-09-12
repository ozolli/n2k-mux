/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/* test_inimerge — vérifie que la fusion préserve les commentaires et n'altère
 * que les valeurs, et qu'elle reste relisible par le parser de config. */

#include "inimerge.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void chk(const char *name, bool cond)
{
    if (cond) return;
    printf("FAIL %s\n", name);
    fails++;
}

static void chk_has(const char *name, const char *hay, const char *needle)
{
    if (strstr(hay, needle)) return;
    printf("FAIL %s : « %s » absent de :\n%s\n", name, needle, hay);
    fails++;
}

static void chk_hasnt(const char *name, const char *hay, const char *needle)
{
    if (!strstr(hay, needle)) return;
    printf("FAIL %s : « %s » présent alors qu'il devait disparaître\n", name, needle);
    fails++;
}

int main(void)
{
    static char out[16384];

    const char *old_ini =
        "# Configuration du bord — NE PAS PERDRE CE COMMENTAIRE\n"
        "\n"
        "[output]\n"
        "talker = II                            ; talker global\n"
        "\n"
        "[sources]\n"
        "; identités relevées sur le bus\n"
        "SCX = 4830123                          ; Furuno SCX-20\n"
        "VER = 917661                           ; Veratron GO\n"
        "\n"
        "[priority]\n"
        "129025          = SCX, VER             ; GLL\n"
        "128267          = min: DST_BB, DST_TB  ; DPT (sécurité)\n"
        "\n"
        "[rate]\n"
        "GLL = 1000\n";

    /* Ce que régénère l'éditeur web : valeurs modifiées, une clé en moins
     * (128267), une clé en plus (129026), une section en plus ([ignore]). */
    const char *new_ini =
        "[output]\n"
        "talker = GP\n"
        "\n[sources]\n"
        "SCX = 4830123\n"
        "VER = 917661\n"
        "\n[priority]\n"
        "129025 = VER, SCX\n"
        "129026 = SCX\n"
        "\n[rate]\n"
        "GLL = 2000\n"
        "\n[ignore]\n"
        "pgn = 262161\n";

    int n = ini_merge(old_ini, new_ini, out, sizeof out);
    chk("fusion réussie", n > 0);
    if (n <= 0) { printf("échecs : %d\n", ++fails); return 1; }

    /* 1. les commentaires survivent */
    chk_has("commentaire d'en-tête", out, "NE PAS PERDRE CE COMMENTAIRE");
    chk_has("commentaire de section", out, "; identités relevées sur le bus");
    chk_has("commentaire de fin de ligne", out, "; Furuno SCX-20");
    chk_has("commentaire conservé sur valeur changée", out, "; talker global");
    chk_has("commentaire conservé sur règle changée", out, "; GLL");

    /* 2. les valeurs changent, l'alignement du '=' est préservé */
    chk_has("valeur talker", out, "talker = GP");
    chk_has("valeur règle + alignement", out, "129025          = VER, SCX");
    chk_has("valeur rate", out, "GLL = 2000");

    /* 3. clé retirée par l'éditeur → ligne supprimée */
    chk_hasnt("clé supprimée", out, "128267");

    /* 4. clé nouvelle ajoutée dans sa section, section nouvelle en fin */
    chk_has("clé ajoutée", out, "129026 = SCX");
    chk_has("section ajoutée", out, "[ignore]");
    chk_has("clé de la section ajoutée", out, "pgn = 262161");
    chk("clé ajoutée DANS sa section",
        strstr(out, "129026 = SCX") > strstr(out, "[priority]") &&
        strstr(out, "129026 = SCX") < strstr(out, "[rate]"));

    /* 5. le résultat reste parsable et porte bien les nouvelles valeurs */
    config_t c;
    chk("fusion relisible par le parser", config_parse_string(&c, out));
    chk("talker relu", strcmp(c.talker, "GP") == 0);
    const cfg_rule_t *r = config_rule(&c, 129025, NULL);
    chk("règle relue", r && r->n_sources == 2 && strcmp(r->sources[0], "VER") == 0);
    chk("règle supprimée absente", config_rule(&c, 128267, NULL) == NULL);
    chk("rate relu", config_rate_ms(&c, "GLL") == 2000);
    chk("ignore relu", config_ignore_pgn(&c, 262161));

    /* 5bis. le commentaire de fin de ligne reste dans SA colonne quand la
     * nouvelle valeur n'est pas plus longue que l'ancienne. */
    chk_has("colonne du commentaire préservée", out,
            "talker = GP                            ; talker global");

    /* 6. idempotence : refusionner le même contenu ne change plus rien */
    static char out2[16384];
    int n2 = ini_merge(out, new_ini, out2, sizeof out2);
    chk("2e fusion réussie", n2 > 0);
    chk("idempotence", n2 > 0 && strcmp(out, out2) == 0);

    /* 7. alias de section : [priorities] doit être apparié à [priority] */
    const char *old_alias = "[priorities]\n129025 = SCX  ; commentaire alias\n";
    const char *new_alias = "[priority]\n129025 = VER\n";
    static char out3[4096];
    chk("fusion alias", ini_merge(old_alias, new_alias, out3, sizeof out3) > 0);
    chk_has("alias : valeur remplacée", out3, "129025 = VER");
    chk_has("alias : commentaire gardé", out3, "; commentaire alias");
    chk_hasnt("alias : pas de section dupliquée", out3, "[priority]");

    /* 8. tampon trop petit → échec franc (l'appelant se rabat sur le brut) */
    char tiny[8];
    chk("débordement détecté", ini_merge(old_ini, new_ini, tiny, sizeof tiny) < 0);

    printf("\n--- Récapitulatif ---\néchecs : %d\n", fails);
    return fails ? 1 : 0;
}
