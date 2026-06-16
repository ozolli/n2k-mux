/*
 * test_sources.c — Vérifie l'aller-retour registre → JSON → vue (sans GTK).
 */

#include "jsonl.h"
#include "registry.h"
#include "sources.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void ok(const char *what, int cond)
{
    if (!cond) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}

static void reg_add(registry_t *r, int src, const char *serial, const char *model)
{
    char line[256];
    snprintf(line, sizeof line,
             "{\"src\":%d,\"pgn\":126996,\"fields\":{\"Model Serial Code\":\"%s\",\"Model ID\":\"%s\"}}",
             src, serial, model);
    jsonl_msg_t m;
    if (jsonl_parse(line, &m)) registry_observe(r, &m);
}

int main(void)
{
    registry_t reg;
    registry_init(&reg);
    reg_add(&reg, 2, "917661", "veratron GO GPS Module");
    reg_add(&reg, 178, "000A520AF6A0", "RDS Sender NMEAD");
    /* un trafic supplémentaire pour incrémenter seen */
    jsonl_msg_t m;
    if (jsonl_parse("{\"src\":2,\"pgn\":129025,\"fields\":{\"Latitude\":47.5}}", &m))
        registry_observe(&reg, &m);

    const char *path = "/tmp/n2kmux_sources_test.json";
    ok("write", sources_write(&reg, path) == 0);

    sources_view_t v;
    int n = sources_load(path, &v);
    ok("load 2 sources", n == 2);

    /* retrouve src 2 */
    const src_entry_t *e2 = NULL, *e178 = NULL;
    for (int i = 0; i < v.n; i++) {
        if (v.e[i].src == 2)   e2 = &v.e[i];
        if (v.e[i].src == 178) e178 = &v.e[i];
    }
    ok("src 2 présent", e2 != NULL);
    ok("src 178 présent", e178 != NULL);
    if (e2) {
        ok("ident 917661", strcmp(e2->ident, "917661") == 0);
        ok("serial 917661", strcmp(e2->serial, "917661") == 0);
        ok("model veratron", strcmp(e2->model, "veratron GO GPS Module") == 0);
        ok("seen >= 2", e2->seen >= 2);   /* 126996 + 129025 */
    }
    if (e178)
        ok("ident DataHub", strcmp(e178->ident, "000A520AF6A0") == 0);

    /* affiche le JSON produit pour contrôle visuel */
    char buf[4096];
    if (sources_to_json(&reg, buf, sizeof buf) > 0)
        fputs(buf, stdout);

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
