/*
 * test_jsonl.c — Vérifie le parser jsonl sur un flux `analyzer -json`.
 *
 * Usage :
 *   cat samples/xxx.raw | rel/.../analyzer -json | ./test_jsonl
 *   cat samples/xxx.raw | rel/.../analyzer -json | ./test_jsonl --fields
 *
 * Sans option : une ligne de résumé par message (src, pgn, description).
 * Avec --fields : détaille aussi chaque champ et son type.
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

int main(int argc, char **argv)
{
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
