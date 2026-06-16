/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * gui.c — Interface GTK3 de n2k-mux (module g).
 *
 * Deux onglets :
 *   - « Sources vues » : liste des équipements publiés par le daemon dans
 *     sources.json (adresse, identité, fabricant, modèle, n° série, vues).
 *     Auto-rafraîchie ; double-clic sur une ligne → copie l'identité (à coller
 *     dans la config). Lecture via le module `sources`.
 *   - « Configuration » : éditeur du fichier INI (texte brut, donc commentaires
 *     préservés). « Valider » réutilise le parser du module `config` (pas de
 *     second parser) ; « Enregistrer » écrit le fichier.
 *
 * La logique de données (sources, config) est dans les modules testés en CLI ;
 * ce fichier ne contient que la couche GTK. Validation visuelle : l'utilisateur
 * lance et fournit des captures.
 *
 * Usage : n2k-mux-gui [config.ini] [--sources CHEMIN]
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>

#include "config.h"
#include "sources.h"

/* Nom du service systemd redémarré par « Enregistrer et redémarrer ». */
#define GUI_SERVICE_NAME "n2k-mux"

enum { COL_SRC, COL_IDENT, COL_MFG, COL_MODEL, COL_SERIAL, COL_SEEN, COL_PGNS, N_COLS };

typedef struct {
    char           cfg_path[512];
    char           sources_path[512];
    char           stats_path[512];
    GtkListStore  *store;
    GtkTextBuffer *ini_buf;
    GtkWidget     *status;
    GtkWidget     *lbl_n2k;     /* résumé charge NMEA 2000 */
    GtkWidget     *lbl_0183;    /* résumé charge NMEA 0183 */
    GtkTextBuffer *stats_buf;   /* détail par PGN / par type */
} app_t;

static void set_status(app_t *a, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(a->status), b);
}

/* ---- fichiers ---- */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *b = g_malloc((size_t)sz + 1);
    size_t r = fread(b, 1, (size_t)sz, f);
    b[r] = '\0';
    fclose(f);
    return b;
}

static gboolean write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (!f) return FALSE;
    size_t len = strlen(data);
    size_t w = fwrite(data, 1, len, f);
    return (fclose(f) == 0) && (w == len);
}

static char *buffer_text(app_t *a)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(a->ini_buf, &s, &e);
    return gtk_text_buffer_get_text(a->ini_buf, &s, &e, FALSE);
}

/* ---- sources ---- */

static void refresh_sources(app_t *a)
{
    sources_view_t v;
    int n = sources_load(a->sources_path, &v);
    gtk_list_store_clear(a->store);
    if (n < 0) {
        set_status(a, "sources.json introuvable : %s", a->sources_path);
        return;
    }
    for (int i = 0; i < v.n; i++) {
        char src[16], seen[24], pgns[512];
        snprintf(src, sizeof src, "%d", v.e[i].src);
        snprintf(seen, sizeof seen, "%lu", v.e[i].seen);
        /* liste des PGN publiés, séparés par des virgules */
        size_t p = 0;
        pgns[0] = '\0';
        for (int j = 0; j < v.e[i].n_pgns && p + 12 < sizeof pgns; j++)
            p += (size_t)snprintf(pgns + p, sizeof pgns - p,
                                  "%s%d", j ? ", " : "", v.e[i].pgns[j]);
        GtkTreeIter it;
        gtk_list_store_append(a->store, &it);
        gtk_list_store_set(a->store, &it,
                           COL_SRC, src, COL_IDENT, v.e[i].ident,
                           COL_MFG, v.e[i].mfg, COL_MODEL, v.e[i].model,
                           COL_SERIAL, v.e[i].serial, COL_SEEN, seen,
                           COL_PGNS, pgns, -1);
    }
    set_status(a, "%d source(s) — %s", v.n, a->sources_path);
}

/* ---- charge (stats.json) ---- */

/* Extrait "key":nombre du JSON (premier match). Défaut si absent. */
static double jnum(const char *s, const char *key, double dflt)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(s, pat);
    return p ? g_ascii_strtod(p + strlen(pat), NULL) : dflt;
}

static void refresh_stats(app_t *a)
{
    char *c = read_file(a->stats_path);
    if (!c) {
        gtk_label_set_text(GTK_LABEL(a->lbl_n2k),
                           "stats.json introuvable — le daemon tourne-t-il avec --stats ?");
        gtk_label_set_text(GTK_LABEL(a->lbl_0183), a->stats_path);
        gtk_text_buffer_set_text(a->stats_buf, "", -1);
        return;
    }

    char l1[256], l2[256];
    snprintf(l1, sizeof l1,
             "NMEA 2000 : %.1f msg/s · ~%.0f trames/s · charge bus ≈ %.1f %%",
             jnum(c, "msg_per_s", 0), jnum(c, "frames_per_s", 0), jnum(c, "bus_load_pct", 0));
    snprintf(l2, sizeof l2,
             "NMEA 0183 : %.1f phrases/s · %.0f o/s · charge ≈ %.1f %% (réf. 4800 bauds)",
             jnum(c, "out_sent_per_s", 0), jnum(c, "out_bytes_per_s", 0), jnum(c, "out_load_pct", 0));
    gtk_label_set_text(GTK_LABEL(a->lbl_n2k), l1);
    gtk_label_set_text(GTK_LABEL(a->lbl_0183), l2);

    /* détail : par PGN (entrée) puis par type de phrase (sortie) */
    GString *g = g_string_new("PGN reçus (Hz) :\n");
    const char *p = strstr(c, "\"pgns\":[");
    if (p) {
        p += strlen("\"pgns\":[");
        const char *end = strchr(p, ']');
        while (p && (!end || p < end)) {
            const char *o = strchr(p, '{');
            if (!o || (end && o > end)) break;
            int    pgn = (int)jnum(o, "pgn", 0);
            double hz  = jnum(o, "hz", 0);
            long   tot = (long)jnum(o, "total", 0);
            g_string_append_printf(g, "  %-7d %7.2f Hz   (total %ld)\n", pgn, hz, tot);
            const char *cl = strchr(o, '}');
            if (!cl) break;
            p = cl + 1;
        }
    }
    g_string_append(g, "\nPhrases 0183 émises (Hz) :\n");
    p = strstr(c, "\"out_types\":[");
    if (p) {
        p += strlen("\"out_types\":[");
        while ((p = strchr(p, '{')) != NULL) {
            char ty[8] = "";
            const char *t = strstr(p, "\"type\":\"");
            if (t) {
                t += strlen("\"type\":\"");
                size_t i = 0;
                while (*t && *t != '"' && i + 1 < sizeof ty) ty[i++] = *t++;
                ty[i] = '\0';
            }
            double hz  = jnum(p, "hz", 0);
            long   tot = (long)jnum(p, "total", 0);
            g_string_append_printf(g, "  %-7s %7.2f Hz   (total %ld)\n", ty, hz, tot);
            const char *cl = strchr(p, '}');
            if (!cl) break;
            p = cl + 1;
        }
    }
    gtk_text_buffer_set_text(a->stats_buf, g->str, -1);
    g_string_free(g, TRUE);
    g_free(c);
}

static gboolean on_timer(gpointer ud)
{
    refresh_sources((app_t *)ud);
    refresh_stats((app_t *)ud);
    return G_SOURCE_CONTINUE;
}

static void on_refresh(GtkWidget *w, gpointer ud)
{
    (void)w;
    refresh_sources((app_t *)ud);
}

/* double-clic : copie l'identité dans le presse-papier */
static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer ud)
{
    (void)col;
    app_t *a = ud;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(m, &it, path)) {
        gchar *ident = NULL;
        gtk_tree_model_get(m, &it, COL_IDENT, &ident, -1);
        if (ident && ident[0]) {
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), ident, -1);
            set_status(a, "identité copiée : %s", ident);
        }
        g_free(ident);
    }
}

/* ---- config INI ---- */

static void load_ini(app_t *a)
{
    char *c = read_file(a->cfg_path);
    if (c) {
        gtk_text_buffer_set_text(a->ini_buf, c, -1);
        g_free(c);
        set_status(a, "config chargée : %s", a->cfg_path);
    } else {
        gtk_text_buffer_set_text(a->ini_buf, "", -1);
        set_status(a, "INI introuvable : %s", a->cfg_path);
    }
}

static void on_reload(GtkWidget *w, gpointer ud)
{
    (void)w;
    load_ini((app_t *)ud);
}

static void on_validate(GtkWidget *w, gpointer ud)
{
    (void)w;
    app_t *a = ud;
    char *txt = buffer_text(a);
    config_t c;
    if (config_parse_string(&c, txt))
        set_status(a, "config valide : %d source(s), %d règle(s)", c.n_sources, c.n_rules);
    else
        set_status(a, "erreur ligne %d : %s", c.err_line, c.err);
    g_free(txt);
}

/* Lance `script` en root via pkexec (popup polkit).
 * Retourne 0 = succès, 1 = exécuté mais échec (auth refusée…), -1 = non lançable. */
static int run_pkexec(const char *script)
{
    gchar *argv[] = { "pkexec", "sh", "-c", (gchar *)script, NULL };
    gint   status = 0;
    GError *e = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                      NULL, NULL, NULL, NULL, &status, &e)) {
        if (e) g_error_free(e);
        return -1;
    }
    return status == 0 ? 0 : 1;
}

/* Valide, écrit l'INI (élévation via pkexec si le fichier appartient à root),
 * et redémarre éventuellement le service — le tout en une seule auth. */
static bool save_config(app_t *a, bool restart)
{
    char *txt = buffer_text(a);
    config_t c;
    if (!config_parse_string(&c, txt)) {
        set_status(a, "NON enregistré — erreur ligne %d : %s", c.err_line, c.err);
        g_free(txt);
        return false;
    }

    bool via_pkexec = false;

    if (write_file(a->cfg_path, txt)) {
        /* écriture directe possible (fichier accessible à l'utilisateur) */
        if (restart && run_pkexec("systemctl restart " GUI_SERVICE_NAME) != 0) {
            set_status(a, "enregistré : %s — mais redémarrage échoué (auth ?)", a->cfg_path);
            g_free(txt);
            return false;
        }
    } else {
        /* droits insuffisants : copier via pkexec depuis un fichier temporaire */
        char *tmp = g_build_filename(g_get_tmp_dir(), "n2k-mux-gui-save.ini", NULL);
        if (!write_file(tmp, txt)) {
            set_status(a, "échec d'écriture du fichier temporaire %s", tmp);
            g_free(tmp); g_free(txt);
            return false;
        }
        char *script = g_strdup_printf("cp '%s' '%s'%s", tmp, a->cfg_path,
                                       restart ? " && systemctl restart " GUI_SERVICE_NAME : "");
        int r = run_pkexec(script);
        g_free(script); unlink(tmp); g_free(tmp);
        if (r == -1) { set_status(a, "pkexec introuvable / non lançable"); g_free(txt); return false; }
        if (r == 1)  { set_status(a, "non enregistré (authentification annulée ou refusée)"); g_free(txt); return false; }
        via_pkexec = true;
    }
    g_free(txt);

    if (restart)
        set_status(a, "enregistré%s et service redémarré.", via_pkexec ? " (pkexec)" : "");
    else
        set_status(a, "enregistré%s : %s", via_pkexec ? " (pkexec)" : "", a->cfg_path);
    return true;
}

static void on_save(GtkWidget *w, gpointer ud)
{
    (void)w;
    save_config((app_t *)ud, false);
}

static void on_save_restart(GtkWidget *w, gpointer ud)
{
    (void)w;
    save_config((app_t *)ud, true);
}

/* ---- construction de l'IHM ---- */

static void add_col(GtkWidget *tree, const char *title, int col)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
}

static GtkWidget *build_sources_tab(app_t *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    a->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
    add_col(tree, "Adresse", COL_SRC);
    add_col(tree, "Identité", COL_IDENT);
    add_col(tree, "Fabricant", COL_MFG);
    add_col(tree, "Modèle", COL_MODEL);
    add_col(tree, "N° série", COL_SERIAL);
    add_col(tree, "Vues", COL_SEEN);
    add_col(tree, "PGNs publiés", COL_PGNS);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), a);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *btn = gtk_button_new_with_label("Rafraîchir");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_refresh), a);
    GtkWidget *hint = gtk_label_new("Double-clic : copier l'identité (à coller dans [sources])");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *build_stats_tab(app_t *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    a->lbl_n2k  = gtk_label_new("NMEA 2000 : —");
    a->lbl_0183 = gtk_label_new("NMEA 0183 : —");
    gtk_widget_set_halign(a->lbl_n2k,  GTK_ALIGN_START);
    gtk_widget_set_halign(a->lbl_0183, GTK_ALIGN_START);

    GtkWidget *detail = gtk_text_view_new();
    a->stats_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(detail));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(detail), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(detail), FALSE);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "textview { font-family: Monospace; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(detail),
                                   GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), detail);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *note = gtk_label_new(
        "Charge bus N2K estimée (messages, pas trames brutes ; ~±15 %). "
        "Charge 0183 = % d'une liaison 4800 bauds. Rafraîchi toutes les 3 s.");
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);

    gtk_box_pack_start(GTK_BOX(box), a->lbl_n2k,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), a->lbl_0183, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *build_config_tab(app_t *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *view = gtk_text_view_new();
    a->ini_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

    /* police à chasse fixe via CSS (API non dépréciée) */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "textview { font-family: Monospace; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(view),
                                   GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *b_reload = gtk_button_new_with_label("Recharger");
    GtkWidget *b_check  = gtk_button_new_with_label("Valider");
    GtkWidget *b_save   = gtk_button_new_with_label("Enregistrer");
    GtkWidget *b_apply  = gtk_button_new_with_label("Enregistrer et redémarrer");
    gtk_widget_set_tooltip_text(b_apply,
        "Écrit l'INI puis redémarre le service (pkexec : demande le mot de passe).");
    g_signal_connect(b_reload, "clicked", G_CALLBACK(on_reload), a);
    g_signal_connect(b_check,  "clicked", G_CALLBACK(on_validate), a);
    g_signal_connect(b_save,   "clicked", G_CALLBACK(on_save), a);
    g_signal_connect(b_apply,  "clicked", G_CALLBACK(on_save_restart), a);
    gtk_box_pack_start(GTK_BOX(bar), b_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_check,  FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), b_apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), b_save, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    return box;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    app_t a;
    memset(&a, 0, sizeof a);
    snprintf(a.cfg_path, sizeof a.cfg_path, "%s", "n2k-mux.ini");
    snprintf(a.sources_path, sizeof a.sources_path, "%s", "/run/n2k-mux/sources.json");
    snprintf(a.stats_path, sizeof a.stats_path, "%s", "/run/n2k-mux/stats.json");
    int start_tab = 0;
    bool cfg_given = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sources") == 0 && i + 1 < argc)
            snprintf(a.sources_path, sizeof a.sources_path, "%s", argv[++i]);
        else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc)
            snprintf(a.stats_path, sizeof a.stats_path, "%s", argv[++i]);
        else if (strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            start_tab = (strcmp(t, "config") == 0) ? 2 :
                        (strcmp(t, "charge") == 0) ? 1 : 0;
        }
        else if (argv[i][0] != '-') {
            snprintf(a.cfg_path, sizeof a.cfg_path, "%s", argv[i]);
            cfg_given = true;
        }
    }

    /* Aucun chemin donné et pas de n2k-mux.ini dans le répertoire courant :
     * se rabattre sur l'emplacement installé par le service. */
    if (!cfg_given && access(a.cfg_path, R_OK) != 0 &&
        access("/etc/n2k-mux/n2k-mux.ini", R_OK) == 0)
        snprintf(a.cfg_path, sizeof a.cfg_path, "%s", "/etc/n2k-mux/n2k-mux.ini");

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "n2k-mux — configuration");
    gtk_window_set_default_size(GTK_WINDOW(win), 820, 600);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_sources_tab(&a), gtk_label_new("Sources vues"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_stats_tab(&a), gtk_label_new("Charge"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_config_tab(&a), gtk_label_new("Configuration"));
    gtk_widget_set_vexpand(nb, TRUE);

    a.status = gtk_label_new("");
    gtk_widget_set_halign(a.status, GTK_ALIGN_START);
    gtk_widget_set_margin_start(a.status, 6);
    gtk_widget_set_margin_top(a.status, 2);
    gtk_widget_set_margin_bottom(a.status, 2);

    gtk_box_pack_start(GTK_BOX(root), nb, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), a.status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), root);

    refresh_sources(&a);
    refresh_stats(&a);
    load_ini(&a);
    g_timeout_add_seconds(3, on_timer, &a);   /* auto-rafraîchit sources + charge */

    gtk_widget_show_all(win);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), start_tab);  /* après show_all */
    gtk_main();
    return 0;
}
