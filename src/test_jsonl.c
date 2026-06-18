/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_jsonl.c — Vérifie le parser jsonl sur un flux `analyzer -json`.
 *
 * Usage :
 *   cat samples/xxx.raw | rel/.../analyzer -json | ./test_jsonl
 *   cat samples/xxx.raw | rel/.../analyzer -json | ./test_jsonl --fields
 *
 * Sans option : une ligne de résumé par message (src, pgn, description).
 * Avec --fields : détaille aussi chaque champ et son type.
 * Avec --selftest : auto-test du typage des champs (ne lit pas stdin).
 * Affiche un récapitulatif final (nb lignes, nb parsées, nb échecs).
 */

#include "jsonl.h"
#include <stdio.h>
#include <string.h>

static const char *typestr(jsonl_valtype_t t)
{
    switch (t) {
        case JSONL_NULL: return "null";
        case JSONL_NUM:  return "num";
        case JSONL_STR:  return "str";
        case JSONL_NV:   return "n/v";
        default:         return "?";
    }
}

/*
 * Auto-test du typage des champs (--selftest) : NE lit PAS stdin, vérifie en
 * dur les cas délicats du parser de valeurs. En particulier la forme canboat
 * {"value":"<chiffres>","key":true} (identifiants type MMSI) qui doit remplir
 * À LA FOIS num et str — sinon la dédup AIS, qui lit "User ID" via get_str /
 * get_num, court-circuite (régression réelle constatée en prod).
 */
static int selftest(void)
{
    int fails = 0;
    jsonl_msg_t m;

    /* MMSI en valeur chaîne clavetée : num ET str renseignés. */
    const char *mmsi_line =
        "{\"src\":31,\"pgn\":129039,"
        "\"fields\":{\"User ID\":{\"value\":\"227211160\",\"key\":true}}}";
    if (!jsonl_parse(mmsi_line, &m)) { fprintf(stderr, "FAIL: parse MMSI\n"); fails++; }
    else {
        const char *s = NULL; double v = 0;
        if (!jsonl_get_str(&m, "User ID", &s) || !s || strcmp(s, "227211160") != 0) {
            fprintf(stderr, "FAIL: MMSI str = %s\n", s ? s : "(null)"); fails++;
        }
        if (!jsonl_get_num(&m, "User ID", &v) || (unsigned long)v != 227211160UL) {
            fprintf(stderr, "FAIL: MMSI num = %g\n", v); fails++;
        }
    }

    /* Enum classique {"value":N,"name":"S"} : num = N, str = nom (inchangé). */
    const char *enum_line =
        "{\"src\":4,\"pgn\":127250,"
        "\"fields\":{\"Reference\":{\"value\":0,\"name\":\"True\"}}}";
    if (!jsonl_parse(enum_line, &m)) { fprintf(stderr, "FAIL: parse enum\n"); fails++; }
    else {
        const char *s = NULL; double v = -1;
        if (!jsonl_get_str(&m, "Reference", &s) || !s || strcmp(s, "True") != 0) {
            fprintf(stderr, "FAIL: enum str = %s\n", s ? s : "(null)"); fails++;
        }
        if (!jsonl_get_num(&m, "Reference", &v) || v != 0) {
            fprintf(stderr, "FAIL: enum num = %g\n", v); fails++;
        }
    }

    fprintf(stderr, "\n--- Auto-test typage ---\néchecs : %d\n", fails);
    return fails > 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest();

    bool show_fields = (argc > 1 && strcmp(argv[1], "--fields") == 0);

    char line[8192];
    unsigned long n_lines = 0, n_ok = 0, n_fail = 0, n_header = 0, n_data = 0;

    while (fgets(line, sizeof line, stdin)) {
        /* ignorer lignes vides */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\0') continue;

        n_lines++;
        jsonl_msg_t m;
        if (!jsonl_parse(line, &m)) {
            n_fail++;
            fprintf(stderr, "FAIL parse: %.80s\n", line);
            continue;
        }
        n_ok++;

        if (m.is_header) {
            n_header++;
            printf("[header] (ligne version canboat ignorée par le daemon)\n");
            continue;
        }
        n_data++;

        printf("src=%-3d pgn=%-6d prio=%-2d  %s\n",
               m.has_src ? m.src : -1,
               m.has_pgn ? m.pgn : -1,
               m.has_prio ? m.prio : -1,
               m.description);

        if (show_fields) {
            for (int i = 0; i < m.n_fields; i++) {
                const jsonl_field_t *f = &m.fields[i];
                printf("      %-32s [%s] ", f->key, typestr(f->type));
                switch (f->type) {
                    case JSONL_NUM: printf("%g\n", f->num); break;
                    case JSONL_STR: printf("\"%s\"\n", f->str); break;
                    case JSONL_NV:  printf("%g / \"%s\"\n", f->num, f->str); break;
                    default:        printf("(null)\n"); break;
                }
            }
        }
    }

    fprintf(stderr,
        "\n--- Récapitulatif ---\n"
        "lignes non vides : %lu\n"
        "  parsées OK     : %lu  (header: %lu, data: %lu)\n"
        "  échecs         : %lu\n",
        n_lines, n_ok, n_header, n_data, n_fail);

    return n_fail > 0 ? 1 : 0;
}
