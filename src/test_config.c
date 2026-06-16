/*
 * test_config.c — Vérifie le parser INI.
 *
 * Parse une config représentative (proche de la table de conversion du bord),
 * puis contrôle : talker, résolution des sources, recherche de règles par
 * (pgn, discriminant) avec spécificité, modes min/fusion, listes [ignore].
 * Teste aussi config_load sur fichier et le signalement d'une erreur de syntaxe.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void ok(const char *what, bool cond)
{
    if (!cond) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}

static void eq_str(const char *what, const char *got, const char *want)
{
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s : attendu \"%s\", obtenu \"%s\"\n",
                what, want, got ? got : "(NULL)");
        failures++;
    }
}

static const char *INI =
    "# Config de test n2k-mux\n"
    "[output]\n"
    "talker = SD   ; talker de sortie\n"
    "\n"
    "[sources]\n"
    "SCX    = 4830123          ; Furuno SCX-20\n"
    "VER    = 917661           ; Veratron GO GPS\n"
    "DH     = 000A520AF6A0     ; DataHub PredictWind\n"
    "MAD    = MB-0001\n"
    "DST_BB = DST-BB-77\n"
    "DST_TB = DST-TB-88\n"
    "AIS    = EMTRAK-1\n"
    "\n"
    "[priority]\n"
    "129025          = SCX, VER, MAD\n"
    "127250/Magnetic = SCX, MAD\n"
    "127250/True     = SCX, MAD\n"
    "130306/Apparent = MAD\n"
    "128267          = min: DST_BB, DST_TB\n"
    "130312/Sea      = DST_BB, DST_TB\n"
    "130312/Outside  = SCX\n"
    "129038          = fusion: AIS, DH\n"
    "\n"
    "[ignore]\n"
    "src = 0\n"
    "pgn = 262161, 262656\n";

int main(void)
{
    config_t c;

    ok("parse OK", config_parse_string(&c, INI));
    if (c.err_line)
        fprintf(stderr, "  (err ligne %d : %s)\n", c.err_line, c.err);

    /* talker */
    eq_str("talker", c.talker, "SD");

    /* sources */
    ok("nb sources = 7", c.n_sources == 7);
    eq_str("ident VER", config_source_ident(&c, "VER"), "917661");
    eq_str("ident DH",  config_source_ident(&c, "DH"),  "000A520AF6A0");
    ok("source inconnue → NULL", config_source_ident(&c, "NOPE") == NULL);

    /* règle simple, ordre de priorité préservé */
    const cfg_rule_t *r = config_rule(&c, 129025, NULL);
    ok("règle 129025 trouvée", r != NULL);
    if (r) {
        ok("129025 mode priority", r->mode == CFG_PICK_PRIORITY);
        ok("129025 3 sources", r->n_sources == 3);
        eq_str("129025[0]", r->sources[0], "SCX");
        eq_str("129025[1]", r->sources[1], "VER");
        eq_str("129025[2]", r->sources[2], "MAD");
    }

    /* discriminant : 127250/Magnetic vs /True */
    r = config_rule(&c, 127250, "Magnetic");
    eq_str("127250/Magnetic source0", r ? r->sources[0] : NULL, "SCX");
    ok("127250/Magnetic 2 sources", r && r->n_sources == 2);

    /* discriminant à préfixe : "True (ground referenced to North)" → /True */
    r = config_rule(&c, 130306, "Apparent");
    ok("130306/Apparent trouvée", r != NULL && r->n_sources == 1);
    eq_str("130306/Apparent source0", r ? r->sources[0] : NULL, "MAD");

    /* mode min (profondeur) */
    r = config_rule(&c, 128267, NULL);
    ok("128267 mode min", r && r->mode == CFG_PICK_MIN);
    ok("128267 2 DST", r && r->n_sources == 2);

    /* 130312 selon Temperature Source (préfixe) */
    r = config_rule(&c, 130312, "Sea Temperature");
    eq_str("130312/Sea source0", r ? r->sources[0] : NULL, "DST_BB");
    r = config_rule(&c, 130312, "Outside Temperature");
    eq_str("130312/Outside source0", r ? r->sources[0] : NULL, "SCX");

    /* mode fusion (AIS) */
    r = config_rule(&c, 129038, NULL);
    ok("129038 mode fusion", r && r->mode == CFG_PICK_FUSION);

    /* pgn sans règle */
    ok("pgn inconnu → NULL", config_rule(&c, 999999, NULL) == NULL);

    /* [ignore] */
    ok("ignore src 0",    config_ignore_src(&c, 0));
    ok("pas ignore src 2", !config_ignore_src(&c, 2));
    ok("ignore pgn 262161", config_ignore_pgn(&c, 262161));
    ok("ignore pgn 262656", config_ignore_pgn(&c, 262656));
    ok("pas ignore 129025", !config_ignore_pgn(&c, 129025));

    /* config_load depuis un fichier temporaire */
    const char *path = "/tmp/n2kmux_test.ini";
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("[output]\ntalker = II\n[sources]\nGPS = 12345\n", f);
        fclose(f);
        config_t c2;
        ok("config_load OK", config_load(&c2, path));
        eq_str("load talker", c2.talker, "II");
        eq_str("load source GPS", config_source_ident(&c2, "GPS"), "12345");
    } else {
        fprintf(stderr, "FAIL création %s\n", path);
        failures++;
    }

    /* erreur de syntaxe signalée */
    config_t c3;
    ok("ligne sans '=' → erreur",
       !config_parse_string(&c3, "[sources]\nceci n'est pas une paire\n"));
    ok("err_line renseigné", c3.err_line == 2);

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
