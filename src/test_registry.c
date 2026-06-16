/*
 * test_registry.c — Construit le registre à partir d'un flux `analyzer -json`
 * et affiche la table src → identité résolue.
 *
 * Usage :
 *   cat samples/xxx.raw | rel/.../analyzer -json | ./test_registry
 *
 * Affiche un enregistrement par device (triés par adresse, puis les détachés),
 * et un récapitulatif sur stderr. Sert à vérifier la résolution d'identité
 * (60928 + 126996) et le suivi des changements d'adresse sur capture réelle.
 */

#include "jsonl.h"
#include "registry.h"
#include <stdio.h>
#include <string.h>

static void print_dev(const reg_device_t *d)
{
    char addr[12], uniq[24];
    if (d->src >= 0)
        snprintf(addr, sizeof addr, "%d", d->src);
    else
        snprintf(addr, sizeof addr, "--");
    if (d->has_unique)
        snprintf(uniq, sizeof uniq, "%lu", (unsigned long)d->unique);
    else
        snprintf(uniq, sizeof uniq, "-");

    printf("src=%-4s ident=%-18s mfg=%-16s model=%-16s serial=%-14s unique=%-9s seen=%lu\n",
           addr,
           d->ident[0]  ? d->ident  : "(?)",
           d->mfg[0]    ? d->mfg    : "-",
           d->model[0]  ? d->model  : "-",
           d->serial[0] ? d->serial : "-",
           uniq,
           d->seen);
}

int main(void)
{
    registry_t reg;
    registry_init(&reg);

    char line[8192];
    unsigned long n_lines = 0, n_ok = 0, n_fail = 0;

    while (fgets(line, sizeof line, stdin)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\0') continue;

        n_lines++;
        jsonl_msg_t m;
        if (!jsonl_parse(line, &m)) { n_fail++; continue; }
        n_ok++;
        registry_observe(&reg, &m);
    }

    printf("=== Registre des équipements (src -> identité stable) ===\n");
    for (int s = 0; s < 256; s++) {
        const reg_device_t *d = registry_by_src(&reg, s);
        if (d)
            print_dev(d);
    }
    /* devices connus mais détachés de toute adresse (évincés) */
    for (int i = 0; i < reg.n; i++)
        if (reg.dev[i].used && reg.dev[i].src == N2K_ADDR_NULL)
            print_dev(&reg.dev[i]);

    fprintf(stderr,
        "\n--- Récapitulatif ---\n"
        "lignes non vides : %lu\n"
        "  parsées OK     : %lu\n"
        "  échecs parse   : %lu\n"
        "devices connus   : %d\n",
        n_lines, n_ok, n_fail, reg.n);

    return n_fail > 0 ? 1 : 0;
}
