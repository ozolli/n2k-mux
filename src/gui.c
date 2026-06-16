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

#include "config.h"
#include "sources.h"

enum { COL_SRC, COL_IDENT, COL_MFG, COL_MODEL, COL_SERIAL, COL_SEEN, N_COLS };

typedef struct {
    char           cfg_path[512];
    char           sources_path[512];
    GtkListStore  *store;
    GtkTextBuffer *ini_buf;
    GtkWidget     *status;
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
        char src[16], seen[24];
        snprintf(src, sizeof src, "%d", v.e[i].src);
        snprintf(seen, sizeof seen, "%lu", v.e[i].seen);
        GtkTreeIter it;
        gtk_list_store_append(a->store, &it);
        gtk_list_store_set(a->store, &it,
                           COL_SRC, src, COL_IDENT, v.e[i].ident,
                           COL_MFG, v.e[i].mfg, COL_MODEL, v.e[i].model,
                           COL_SERIAL, v.e[i].serial, COL_SEEN, seen, -1);
    }
    set_status(a, "%d source(s) — %s", v.n, a->sources_path);
}

static gboolean on_timer(gpointer ud)
{
    refresh_sources((app_t *)ud);
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

static void on_save(GtkWidget *w, gpointer ud)
{
    (void)w;
    app_t *a = ud;
    char *txt = buffer_text(a);
    config_t c;
    if (!config_parse_string(&c, txt)) {
        set_status(a, "NON enregistré — erreur ligne %d : %s", c.err_line, c.err);
    } else if (write_file(a->cfg_path, txt)) {
        set_status(a, "enregistré : %s", a->cfg_path);
    } else {
        set_status(a, "échec d'écriture : %s", a->cfg_path);
    }
    g_free(txt);
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
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
    add_col(tree, "Adresse", COL_SRC);
    add_col(tree, "Identité", COL_IDENT);
    add_col(tree, "Fabricant", COL_MFG);
    add_col(tree, "Modèle", COL_MODEL);
    add_col(tree, "N° série", COL_SERIAL);
    add_col(tree, "Vues", COL_SEEN);
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
    g_signal_connect(b_reload, "clicked", G_CALLBACK(on_reload), a);
    g_signal_connect(b_check,  "clicked", G_CALLBACK(on_validate), a);
    g_signal_connect(b_save,   "clicked", G_CALLBACK(on_save), a);
    gtk_box_pack_start(GTK_BOX(bar), b_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_check,  FALSE, FALSE, 0);
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
    int start_tab = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sources") == 0 && i + 1 < argc)
            snprintf(a.sources_path, sizeof a.sources_path, "%s", argv[++i]);
        else if (strcmp(argv[i], "--tab") == 0 && i + 1 < argc)
            start_tab = (strcmp(argv[++i], "config") == 0) ? 1 : 0;
        else if (argv[i][0] != '-')
            snprintf(a.cfg_path, sizeof a.cfg_path, "%s", argv[i]);
    }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "n2k-mux — configuration");
    gtk_window_set_default_size(GTK_WINDOW(win), 820, 600);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_sources_tab(&a), gtk_label_new("Sources vues"));
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
    load_ini(&a);
    g_timeout_add_seconds(3, on_timer, &a);   /* auto-rafraîchit les sources */

    gtk_widget_show_all(win);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), start_tab);  /* après show_all */
    gtk_main();
    return 0;
}
