#include <gtk/gtk.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <json-glib/json-glib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define SOCKET_PATH "/tmp/waybar-ytmusic.sock"
#define CTL_SCRIPT "/home/denilson-oliveira/dev/waybar-ytmusic/ytmusic_ctl.py"

typedef struct {
    GtkWidget *window;
    GtkWidget *main_box;
    GtkWidget *expanded_box;
    GtkWidget *btn_expand;
    gboolean expanded;
    gboolean home_loaded_once;

    GtkWidget *cover_img;
    char current_cover_path[256];
    char expected_large_cover[256];
    GList *pending_images;

    GtkWidget *title_lbl;
    GtkWidget *artist_lbl;
    GtkWidget *album_lbl;

    GtkWidget *pos_scale;
    GtkWidget *pos_lbl;
    GtkWidget *dur_lbl;
    gboolean seeking;
    gint64 seek_cooldown_until;
    double current_dur;

    GtkWidget *btn_shuffle;
    GtkWidget *btn_prev;
    GtkWidget *btn_play;
    GtkWidget *btn_next;
    GtkWidget *btn_repeat;
    GtkWidget *btn_like;
    GtkWidget *btn_radio;
    GtkWidget *vol_scale;
    gboolean updating_vol;

    GtkWidget *stack;
    GtkWidget *switcher;
    char current_state[32];

    /* Home Tab */
    GtkWidget *home_status_lbl;
    GtkWidget *home_sections_box;

    /* Search Tab */
    GtkWidget *search_entry;
    GtkWidget *search_suggestions_box;
    GtkWidget *btn_clear_search_hist;
    guint search_debounce_id;
    guint search_seq;
    gboolean suppress_search_changed;
    GtkWidget *filter_songs_btn;
    GtkWidget *filter_albums_btn;
    GtkWidget *filter_playlists_btn;
    char current_filter[32];
    GtkWidget *search_status_lbl;
    GtkWidget *search_back_box;
    GtkWidget *search_collection_lbl;
    GtkWidget *search_list;
    JsonArray *current_collection_tracks;

    /* Queue Tab */
    GtkWidget *queue_count_lbl;
    GtkWidget *queue_list;
    GtkWidget *queue_scroll;
    int last_queue_len;
    int last_queue_idx;
    char last_video_id[64];
    gboolean q_dragging;
    gboolean q_drag_moved;
    gboolean q_swipe_right;
    int q_drag_from_idx;
    int q_drag_to_idx;
    double q_drag_start_x_root;
    double q_drag_start_y_root;
    double q_last_dx;
    GtkWidget *q_drag_source_box;
    GtkWidget *q_drag_grip_lbl;
    GtkWidget *q_drag_target_box;

    /* Liked Tab */
    GtkWidget *liked_status_lbl;
    GtkWidget *liked_list;
    JsonArray *liked_tracks_cache;

    /* Library Tab */
    GtkWidget *lib_status_lbl;
    GtkWidget *lib_playlists_list;
    GtkWidget *lib_history_list;

    /* Account & Theme Tab */
    GtkWidget *auth_badge_lbl;
    GtkWidget *auth_msg_lbl;
    GtkWidget *headers_textview;
    GtkWidget *theme_btns[8];
    GtkCssProvider *css_provider;
    char current_theme_id[32];

    /* Current Track Cache */
    JsonObject *current_track_obj;
    guint poll_timer_id;
} YTMusicApp;

static YTMusicApp app = {0};

/* Helper: Send JSON string to daemon UNIX socket and return parsed JsonNode* */
static JsonNode *send_daemon_request(JsonBuilder *builder) {
    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gsize len = 0;
    gchar *json_str = json_generator_to_data(gen, &len);
    json_node_unref(root);
    g_object_unref(gen);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_free(json_str);
        return NULL;
    }

    struct timeval tv;
    tv.tv_sec = 12;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        /* Try starting daemon via ctl script */
        int unused = system(CTL_SCRIPT " start-daemon >/dev/null 2>&1");
        (void)unused;
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            if (fd >= 0) close(fd);
            g_free(json_str);
            return NULL;
        }
    }

    ssize_t w1 = write(fd, json_str, len);
    ssize_t w2 = write(fd, "\n", 1);
    (void)w1;
    (void)w2;
    g_free(json_str);

    GString *resp_str = g_string_sized_new(4096);
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        g_string_append_len(resp_str, buf, n);
        if (memchr(buf, '\n', (size_t)n) != NULL) {
            break;
        }
    }
    close(fd);

    if (resp_str->len == 0) {
        g_string_free(resp_str, TRUE);
        return NULL;
    }

    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, resp_str->str, (gssize)resp_str->len, &err)) {
        if (err) g_error_free(err);
        g_object_unref(parser);
        g_string_free(resp_str, TRUE);
        return NULL;
    }

    JsonNode *res_node = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    g_string_free(resp_str, TRUE);
    return res_node;
}

static JsonNode *send_simple_cmd(const char *cmd_name) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, cmd_name);
    json_builder_end_object(b);
    return send_daemon_request(b);
}

static const char *j_str(JsonObject *obj, const char *key, const char *fallback) {
    if (!obj || !json_object_has_member(obj, key)) return fallback;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return fallback;
    const char *v = json_object_get_string_member(obj, key);
    return v ? v : fallback;
}

static double j_double(JsonObject *obj, const char *key, double fallback) {
    if (!obj || !json_object_has_member(obj, key)) return fallback;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return fallback;
    return json_object_get_double_member(obj, key);
}

static gint64 j_int(JsonObject *obj, const char *key, gint64 fallback) {
    if (!obj || !json_object_has_member(obj, key)) return fallback;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return fallback;
    return json_object_get_int_member(obj, key);
}

static gboolean j_bool(JsonObject *obj, const char *key, gboolean fallback) {
    if (!obj || !json_object_has_member(obj, key)) return fallback;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return fallback;
    return json_object_get_boolean_member(obj, key);
}

static void format_mmss(double sec, char *out, size_t out_sz) {
    int s = (int)(sec > 0 ? sec : 0);
    int m = s / 60;
    s = s % 60;
    snprintf(out, out_sz, "%02d:%02d", m, s);
}

static void clear_container(GtkWidget *container) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);
}

static void set_image_from_path(GtkImage *img, const char *path, int size) {
    if (!path || path[0] == '\0') return;
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, &err);
    if (pb) {
        gtk_image_set_from_pixbuf(img, pb);
        g_object_unref(pb);
    } else if (err) {
        g_error_free(err);
    }
}

static void on_pending_img_destroyed(GtkWidget *w, gpointer data) {
    (void)data;
    app.pending_images = g_list_remove(app.pending_images, w);
}

static void register_pending_cover(GtkWidget *img, const char *expected_path) {
    if (!expected_path || !expected_path[0]) return;
    g_object_set_data_full(G_OBJECT(img), "expected-cover", g_strdup(expected_path), g_free);
    g_signal_connect(img, "destroy", G_CALLBACK(on_pending_img_destroyed), NULL);
    app.pending_images = g_list_prepend(app.pending_images, img);
}

static void check_pending_covers(void) {
    GList *iter = app.pending_images;
    while (iter != NULL) {
        GList *next = iter->next;
        GtkWidget *img = GTK_WIDGET(iter->data);
        const char *exp_path = (const char *)g_object_get_data(G_OBJECT(img), "expected-cover");
        if (exp_path && g_file_test(exp_path, G_FILE_TEST_EXISTS)) {
            set_image_from_path(GTK_IMAGE(img), exp_path, 40);
            app.pending_images = g_list_delete_link(app.pending_images, iter);
        }
        iter = next;
    }

    if (app.expected_large_cover[0] != '\0' &&
        strcmp(app.current_cover_path, app.expected_large_cover) != 0 &&
        g_file_test(app.expected_large_cover, G_FILE_TEST_EXISTS)) {
        memcpy(app.current_cover_path, app.expected_large_cover, sizeof(app.current_cover_path));
        set_image_from_path(GTK_IMAGE(app.cover_img), app.current_cover_path, app.expanded ? 84 : 56);
    }
}

static void set_expanded_mode(gboolean expand);
static void refresh_search_history_ui(void);

/* Forward declarations */
static void refresh_status_ui(JsonObject *st);
static void refresh_home_tab(gboolean force_refresh);
static void refresh_queue_tab(void);
static void refresh_liked_tab(void);
static void refresh_library_tab(void);
static void populate_track_rows(GtkWidget *listbox, JsonArray *items, gboolean is_queue);

static void poll_and_update(void) {
    check_pending_covers();
    JsonNode *node = send_simple_cmd("status");
    if (!node) return;
    if (JSON_NODE_HOLDS_OBJECT(node)) {
        refresh_status_ui(json_node_get_object(node));
    }
    json_node_unref(node);
}

static void refresh_status_ui(JsonObject *st) {
    if (!st) return;

    const char *state = j_str(st, "state", "stopped");
    strncpy(app.current_state, state, sizeof(app.current_state) - 1);
    double pos = j_double(st, "position", 0.0);
    double dur = j_double(st, "duration", 0.0);
    double vol = j_double(st, "volume", 80.0);
    gboolean shuf = j_bool(st, "shuffle", FALSE);
    gboolean radio = j_bool(st, "radio", TRUE);
    const char *repeat_mode = j_str(st, "repeat", "off");
    gboolean auth = j_bool(st, "authenticated", FALSE);
    const char *auth_src = j_str(st, "auth_source", "Modo Visitante");
    int q_len = (int)j_int(st, "queue_length", 0);
    int q_idx = (int)j_int(st, "queue_index", -1);
    const char *def_cover = j_str(st, "default_cover_large", "/tmp/waybar-ytmusic-covers/default_96.png");

    /* Update play button icon */
    if (strcmp(state, "playing") == 0) {
        gtk_button_set_label(GTK_BUTTON(app.btn_play), "󰏤");
    } else if (strcmp(state, "loading") == 0) {
        gtk_button_set_label(GTK_BUTTON(app.btn_play), "󱑽");
    } else {
        gtk_button_set_label(GTK_BUTTON(app.btn_play), "󰐊");
    }

    /* Toggle classes on shuffle and radio */
    GtkStyleContext *shuf_ctx = gtk_widget_get_style_context(app.btn_shuffle);
    if (shuf) gtk_style_context_add_class(shuf_ctx, "toggle-on");
    else gtk_style_context_remove_class(shuf_ctx, "toggle-on");

    GtkStyleContext *rad_ctx = gtk_widget_get_style_context(app.btn_radio);
    if (radio) gtk_style_context_add_class(rad_ctx, "toggle-on");
    else gtk_style_context_remove_class(rad_ctx, "toggle-on");

    GtkStyleContext *rep_ctx = gtk_widget_get_style_context(app.btn_repeat);
    if (strcmp(repeat_mode, "one") == 0) {
        gtk_button_set_label(GTK_BUTTON(app.btn_repeat), "󰑘 1");
        gtk_style_context_add_class(rep_ctx, "toggle-on");
        gtk_widget_set_tooltip_text(app.btn_repeat, "Repetindo a música atual (clique para repetir a fila)");
    } else if (strcmp(repeat_mode, "all") == 0) {
        gtk_button_set_label(GTK_BUTTON(app.btn_repeat), "󰑖 All");
        gtk_style_context_add_class(rep_ctx, "toggle-on");
        gtk_widget_set_tooltip_text(app.btn_repeat, "Repetindo toda a fila (clique para desativar)");
    } else {
        gtk_button_set_label(GTK_BUTTON(app.btn_repeat), "󰑖");
        gtk_style_context_remove_class(rep_ctx, "toggle-on");
        gtk_widget_set_tooltip_text(app.btn_repeat, "Repetição desligada (clique para repetir a música)");
    }

    /* Update volume slider */
    if (!app.updating_vol) {
        app.updating_vol = TRUE;
        gtk_range_set_value(GTK_RANGE(app.vol_scale), vol);
        app.updating_vol = FALSE;
    }

    /* Update progress bar */
    app.current_dur = dur > 1.0 ? dur : 100.0;
    gtk_range_set_range(GTK_RANGE(app.pos_scale), 0.0, app.current_dur);
    char b2[16];
    format_mmss(dur, b2, sizeof(b2));
    gtk_label_set_text(GTK_LABEL(app.dur_lbl), b2);

    if (!app.seeking && g_get_monotonic_time() >= app.seek_cooldown_until) {
        gtk_range_set_value(GTK_RANGE(app.pos_scale), pos);
        char b1[16];
        format_mmss(pos, b1, sizeof(b1));
        gtk_label_set_text(GTK_LABEL(app.pos_lbl), b1);
    }

    /* Update auth badge */
    char auth_markup[256];
    snprintf(auth_markup, sizeof(auth_markup),
             "<span color='%s'><b>%s %s</b></span>",
             auth ? "#a6e3a1" : "#f9e2af",
             auth ? "󰄬" : "󰀨",
             auth_src);
    gtk_label_set_markup(GTK_LABEL(app.auth_badge_lbl), auth_markup);

    /* Update Track Card */
    if (json_object_has_member(st, "track") && !json_node_is_null(json_object_get_member(st, "track"))) {
        JsonObject *tr = json_object_get_object_member(st, "track");
        if (app.current_track_obj) json_object_unref(app.current_track_obj);
        app.current_track_obj = json_object_ref(tr);

        const char *vid = j_str(tr, "videoId", "");
        const char *title = j_str(tr, "title", "Sem título");
        const char *artist = j_str(tr, "artist", "YouTube Music");
        const char *album = j_str(tr, "album", "");
        const char *cover = j_str(tr, "cover_large", def_cover);
        const char *exp_large = j_str(tr, "expected_cover_large", "");
        gboolean liked = j_bool(tr, "liked", FALSE);

        gtk_label_set_text(GTK_LABEL(app.title_lbl), title);
        gtk_label_set_text(GTK_LABEL(app.artist_lbl), artist);
        if (strcmp(state, "loading") == 0) {
            gtk_label_set_text(GTK_LABEL(app.album_lbl), "Carregando stream de áudio...");
        } else if (!app.expanded) {
            gtk_label_set_text(GTK_LABEL(app.album_lbl),
                               album[0] ? album : "Clique aqui para expandir o player");
        } else {
            gtk_label_set_text(GTK_LABEL(app.album_lbl), album[0] ? album : "Tocando no YouTube Music");
        }

        GtkStyleContext *like_ctx = gtk_widget_get_style_context(app.btn_like);
        if (liked) {
            gtk_button_set_label(GTK_BUTTON(app.btn_like), "♥");
            gtk_style_context_add_class(like_ctx, "liked-btn");
        } else {
            gtk_button_set_label(GTK_BUTTON(app.btn_like), "♡");
            gtk_style_context_remove_class(like_ctx, "liked-btn");
        }

        const char *use_cover = (exp_large[0] && g_file_test(exp_large, G_FILE_TEST_EXISTS)) ? exp_large : cover;
        strncpy(app.current_cover_path, use_cover, sizeof(app.current_cover_path) - 1);
        strncpy(app.expected_large_cover, exp_large, sizeof(app.expected_large_cover) - 1);
        set_image_from_path(GTK_IMAGE(app.cover_img), use_cover, app.expanded ? 84 : 56);

        if (strcmp(vid, app.last_video_id) != 0 || q_len != app.last_queue_len || q_idx != app.last_queue_idx) {
            strncpy(app.last_video_id, vid, sizeof(app.last_video_id) - 1);
            app.last_queue_len = q_len;
            app.last_queue_idx = q_idx;
            if (app.expanded && !app.q_dragging) {
                refresh_queue_tab();
            }
        }
    } else {
        if (app.current_track_obj) {
            json_object_unref(app.current_track_obj);
            app.current_track_obj = NULL;
        }
        gtk_label_set_text(GTK_LABEL(app.title_lbl), "Nenhuma música tocando");
        gtk_label_set_text(GTK_LABEL(app.artist_lbl),
                           app.expanded ? "Pesquise uma música ou escolha da sua biblioteca"
                                        : "Clique aqui ou em Expandir para buscar músicas");
        gtk_label_set_text(GTK_LABEL(app.album_lbl), "");
        gtk_button_set_label(GTK_BUTTON(app.btn_like), "♡");
        gtk_style_context_remove_class(gtk_widget_get_style_context(app.btn_like), "liked-btn");
        strncpy(app.current_cover_path, def_cover, sizeof(app.current_cover_path) - 1);
        app.expected_large_cover[0] = '\0';
        set_image_from_path(GTK_IMAGE(app.cover_img), def_cover, app.expanded ? 84 : 56);
    }
}

/* --- Player Controls Callbacks --- */
static void on_play_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *n = send_simple_cmd("toggle");
    if (n) {
        if (JSON_NODE_HOLDS_OBJECT(n)) refresh_status_ui(json_node_get_object(n));
        json_node_unref(n);
    }
}

static void on_prev_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *n = send_simple_cmd("prev");
    if (n) {
        if (JSON_NODE_HOLDS_OBJECT(n)) refresh_status_ui(json_node_get_object(n));
        json_node_unref(n);
    }
}

static void on_next_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *n = send_simple_cmd("next");
    if (n) {
        if (JSON_NODE_HOLDS_OBJECT(n)) refresh_status_ui(json_node_get_object(n));
        json_node_unref(n);
    }
}

static void on_shuffle_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *n = send_simple_cmd("toggle_shuffle");
    if (n) {
        if (JSON_NODE_HOLDS_OBJECT(n)) refresh_status_ui(json_node_get_object(n));
        json_node_unref(n);
    }
}

static void on_radio_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *n = send_simple_cmd("toggle_radio");
    if (n) {
        if (JSON_NODE_HOLDS_OBJECT(n)) refresh_status_ui(json_node_get_object(n));
        json_node_unref(n);
    }
}

static void on_repeat_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *n = send_simple_cmd("toggle_repeat");
    if (n) {
        if (JSON_NODE_HOLDS_OBJECT(n)) refresh_status_ui(json_node_get_object(n));
        json_node_unref(n);
    }
}

static void on_like_current_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    if (!app.current_track_obj) return;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "toggle_like");
    json_builder_set_member_name(b, "track");
    JsonNode *tcopy = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(tcopy, app.current_track_obj);
    json_builder_add_value(b, tcopy);
    json_builder_end_object(b);

    JsonNode *res = send_daemon_request(b);
    if (res) json_node_unref(res);
    poll_and_update();
}

static double compute_pos_from_x(GtkWidget *w, double x) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(w, &alloc);
    double pad = 8.0;
    double usable = (double)alloc.width - 2.0 * pad;
    if (usable <= 1.0) usable = 1.0;
    double ratio = (x - pad) / usable;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    double max_v = app.current_dur > 1.0 ? app.current_dur : 100.0;
    return ratio * max_v;
}

static gboolean on_pos_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)data;
    if (ev->button == 1) {
        app.seeking = TRUE;
        double target = compute_pos_from_x(w, ev->x);
        gtk_range_set_value(GTK_RANGE(w), target);
        char b1[16];
        format_mmss(target, b1, sizeof(b1));
        gtk_label_set_text(GTK_LABEL(app.pos_lbl), b1);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_pos_motion_notify(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)data;
    if (app.seeking && (ev->state & GDK_BUTTON1_MASK)) {
        double target = compute_pos_from_x(w, ev->x);
        gtk_range_set_value(GTK_RANGE(w), target);
        char b1[16];
        format_mmss(target, b1, sizeof(b1));
        gtk_label_set_text(GTK_LABEL(app.pos_lbl), b1);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_pos_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)data;
    if (!app.seeking) return FALSE;
    double target = compute_pos_from_x(w, ev->x);
    gtk_range_set_value(GTK_RANGE(w), target);
    char b1[16];
    format_mmss(target, b1, sizeof(b1));
    gtk_label_set_text(GTK_LABEL(app.pos_lbl), b1);

    app.seeking = FALSE;
    app.seek_cooldown_until = g_get_monotonic_time() + 1200000; /* 1.2s cooldown */

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "seek");
    json_builder_set_member_name(b, "position");
    json_builder_add_double_value(b, target);
    json_builder_end_object(b);
    JsonNode *res = send_daemon_request(b);
    if (res) {
        if (JSON_NODE_HOLDS_OBJECT(res)) refresh_status_ui(json_node_get_object(res));
        json_node_unref(res);
    }
    return TRUE;
}

static void on_vol_value_changed(GtkRange *range, gpointer data) {
    (void)data;
    if (app.updating_vol) return;
    double v = gtk_range_get_value(range);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "volume");
    json_builder_set_member_name(b, "value");
    json_builder_add_double_value(b, v);
    json_builder_end_object(b);
    JsonNode *res = send_daemon_request(b);
    if (res) json_node_unref(res);
}

/* --- Row Action Callbacks --- */
static void on_row_play_track(GtkButton *btn, gpointer user_data) {
    (void)btn;
    JsonObject *track_obj = (JsonObject *)user_data;
    if (!track_obj) return;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "play_track");
    json_builder_set_member_name(b, "clear_queue");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "track");
    JsonNode *tcopy = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(tcopy, track_obj);
    json_builder_add_value(b, tcopy);
    json_builder_end_object(b);

    JsonNode *res = send_daemon_request(b);
    if (res) {
        if (JSON_NODE_HOLDS_OBJECT(res)) refresh_status_ui(json_node_get_object(res));
        json_node_unref(res);
    }
}

static void on_row_add_queue(GtkButton *btn, gpointer user_data) {
    JsonObject *track_obj = (JsonObject *)user_data;
    if (!track_obj) return;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "add_to_queue");
    json_builder_set_member_name(b, "track");
    JsonNode *tcopy = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(tcopy, track_obj);
    json_builder_add_value(b, tcopy);
    json_builder_end_object(b);

    JsonNode *res = send_daemon_request(b);
    if (res) json_node_unref(res);
    gtk_button_set_label(btn, "✓");
    poll_and_update();
}

static void on_row_toggle_like(GtkButton *btn, gpointer user_data) {
    JsonObject *track_obj = (JsonObject *)user_data;
    if (!track_obj) return;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "toggle_like");
    json_builder_set_member_name(b, "track");
    JsonNode *tcopy = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(tcopy, track_obj);
    json_builder_add_value(b, tcopy);
    json_builder_end_object(b);

    JsonNode *res = send_daemon_request(b);
    if (res && JSON_NODE_HOLDS_OBJECT(res)) {
        gboolean liked = j_bool(json_node_get_object(res), "liked", FALSE);
        gtk_button_set_label(btn, liked ? "♥" : "♡");
        GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(btn));
        if (liked) gtk_style_context_add_class(ctx, "liked-btn");
        else gtk_style_context_remove_class(ctx, "liked-btn");
    }
    if (res) json_node_unref(res);
    poll_and_update();
}

static void play_queue_index_now(int idx) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "play_queue_index");
    json_builder_set_member_name(b, "index");
    json_builder_add_int_value(b, idx);
    json_builder_end_object(b);
    JsonNode *res = send_daemon_request(b);
    if (res) {
        if (JSON_NODE_HOLDS_OBJECT(res)) refresh_status_ui(json_node_get_object(res));
        json_node_unref(res);
    }
    refresh_queue_tab();
}

static void remove_queue_index_now(int idx) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "remove_queue_index");
    json_builder_set_member_name(b, "index");
    json_builder_add_int_value(b, idx);
    json_builder_end_object(b);
    JsonNode *res = send_daemon_request(b);
    if (res) json_node_unref(res);
    refresh_queue_tab();
    poll_and_update();
}

typedef struct {
    GtkWidget *hbox;
    int idx;
    int step;
    int start_margin;
} SwipeRemoveAnim;

static gboolean animate_swipe_remove_step(gpointer data) {
    SwipeRemoveAnim *anim = (SwipeRemoveAnim *)data;
    anim->step++;
    if (GTK_IS_WIDGET(anim->hbox)) {
        int margin = anim->start_margin + anim->step * 22;
        double opac = 1.0 - ((double)anim->step / 8.0);
        if (opac < 0.02) opac = 0.02;
        gtk_widget_set_margin_start(anim->hbox, margin);
        gtk_widget_set_opacity(anim->hbox, opac);
    }
    if (anim->step >= 8) {
        if (GTK_IS_WIDGET(anim->hbox)) {
            gtk_widget_set_margin_start(anim->hbox, 0);
            g_object_unref(anim->hbox);
        }
        int rm_idx = anim->idx;
        g_free(anim);
        remove_queue_index_now(rm_idx);
        return FALSE;
    }
    return TRUE;
}

static void move_queue_item_to(int from_idx, int to_idx) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "move_queue_index");
    json_builder_set_member_name(b, "from_index");
    json_builder_add_int_value(b, from_idx);
    json_builder_set_member_name(b, "to_index");
    json_builder_add_int_value(b, to_idx);
    json_builder_end_object(b);
    JsonNode *res = send_daemon_request(b);
    if (res) json_node_unref(res);
    refresh_queue_tab();
    poll_and_update();
}

static gboolean on_queue_row_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 1) {
        app.q_dragging = TRUE;
        app.q_drag_moved = FALSE;
        app.q_swipe_right = FALSE;
        app.q_drag_from_idx = GPOINTER_TO_INT(user_data);
        app.q_drag_to_idx = app.q_drag_from_idx;
        app.q_drag_start_x_root = ev->x_root;
        app.q_drag_start_y_root = ev->y_root;
        app.q_last_dx = 0.0;
        app.q_drag_source_box = GTK_WIDGET(g_object_get_data(G_OBJECT(w), "row-hbox"));
        app.q_drag_grip_lbl = GTK_WIDGET(g_object_get_data(G_OBJECT(w), "grip-lbl"));
        app.q_drag_target_box = NULL;
        return TRUE;
    }
    return FALSE;
}

static gboolean on_queue_row_motion(GtkWidget *w, GdkEventMotion *ev, gpointer user_data) {
    (void)user_data;
    if (!app.q_dragging) return FALSE;
    double dx = ev->x_root - app.q_drag_start_x_root;
    double dy = ev->y_root - app.q_drag_start_y_root;
    app.q_last_dx = dx;

    if (!app.q_drag_moved && (fabs(dx) > 6.0 || fabs(dy) > 6.0)) {
        app.q_drag_moved = TRUE;
        if (dx > 6.0 && fabs(dx) > fabs(dy) * 1.15) {
            app.q_swipe_right = TRUE;
            if (app.q_drag_source_box) {
                gtk_style_context_add_class(gtk_widget_get_style_context(app.q_drag_source_box), "swiping-remove-row");
            }
        } else {
            app.q_swipe_right = FALSE;
            if (app.q_drag_source_box) {
                gtk_style_context_add_class(gtk_widget_get_style_context(app.q_drag_source_box), "dragging-row");
            }
        }
    }

    if (app.q_drag_moved && app.q_swipe_right) {
        int shift = dx > 0.0 ? (int)dx : 0;
        if (shift > 180) shift = 180;
        if (app.q_drag_source_box) {
            gtk_widget_set_margin_start(app.q_drag_source_box, shift);
            double opac = 1.0 - ((double)shift / 260.0);
            if (opac < 0.35) opac = 0.35;
            gtk_widget_set_opacity(app.q_drag_source_box, opac);

            GtkStyleContext *sctx = gtk_widget_get_style_context(app.q_drag_source_box);
            if (shift >= 70) {
                gtk_style_context_add_class(sctx, "swiping-remove-armed");
                if (app.q_drag_grip_lbl) {
                    gtk_label_set_text(GTK_LABEL(app.q_drag_grip_lbl), "󰆴 Remover");
                }
            } else {
                gtk_style_context_remove_class(sctx, "swiping-remove-armed");
                if (app.q_drag_grip_lbl) {
                    gtk_label_set_text(GTK_LABEL(app.q_drag_grip_lbl), "󰆴");
                }
            }
        }
        return TRUE;
    }

    if (app.q_drag_moved && !app.q_swipe_right && app.queue_list) {
        int lx = 0, ly = 0;
        if (gtk_widget_translate_coordinates(w, app.queue_list, (int)ev->x, (int)ev->y, &lx, &ly)) {
            /* Auto-scroll when dragging near top/bottom of scroll viewport */
            if (app.queue_scroll) {
                GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(app.queue_scroll));
                if (vadj) {
                    double val = gtk_adjustment_get_value(vadj);
                    double page = gtk_adjustment_get_page_size(vadj);
                    double upper = gtk_adjustment_get_upper(vadj);
                    double rel_y = (double)ly - val;
                    if (rel_y < 28.0 && val > 0.0) {
                        double nv = val - 14.0;
                        gtk_adjustment_set_value(vadj, nv > 0.0 ? nv : 0.0);
                    } else if (rel_y > page - 28.0 && val + page < upper) {
                        double nv = val + 14.0;
                        double max_v = upper - page;
                        gtk_adjustment_set_value(vadj, nv < max_v ? nv : max_v);
                    }
                }
            }

            GtkListBoxRow *trow = gtk_list_box_get_row_at_y(GTK_LIST_BOX(app.queue_list), ly);
            if (trow) {
                int tidx = gtk_list_box_row_get_index(trow);
                GtkWidget *thbox = GTK_WIDGET(g_object_get_data(G_OBJECT(trow), "row-hbox"));
                if (app.q_drag_target_box && app.q_drag_target_box != thbox) {
                    GtkStyleContext *old_ctx = gtk_widget_get_style_context(app.q_drag_target_box);
                    gtk_style_context_remove_class(old_ctx, "drop-target-above");
                    gtk_style_context_remove_class(old_ctx, "drop-target-below");
                }
                app.q_drag_to_idx = tidx;
                app.q_drag_target_box = thbox;
                if (thbox && tidx != app.q_drag_from_idx) {
                    GtkStyleContext *new_ctx = gtk_widget_get_style_context(thbox);
                    gtk_style_context_remove_class(new_ctx, "drop-target-above");
                    gtk_style_context_remove_class(new_ctx, "drop-target-below");
                    gtk_style_context_add_class(new_ctx,
                        tidx < app.q_drag_from_idx ? "drop-target-above" : "drop-target-below");
                }
            }
        }
        return TRUE;
    }
    return FALSE;
}

static gboolean on_queue_row_button_release(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    (void)w; (void)ev; (void)user_data;
    if (!app.q_dragging) return FALSE;
    app.q_dragging = FALSE;

    if (app.q_drag_target_box) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(app.q_drag_target_box);
        gtk_style_context_remove_class(ctx, "drop-target-above");
        gtk_style_context_remove_class(ctx, "drop-target-below");
        app.q_drag_target_box = NULL;
    }

    if (app.q_drag_moved && app.q_swipe_right) {
        GtkWidget *sbox = app.q_drag_source_box;
        app.q_drag_source_box = NULL;
        if (app.q_last_dx >= 70.0 && sbox) {
            GtkStyleContext *sctx = gtk_widget_get_style_context(sbox);
            gtk_style_context_add_class(sctx, "swiping-remove-armed");
            if (app.q_drag_grip_lbl) {
                gtk_label_set_text(GTK_LABEL(app.q_drag_grip_lbl), "󰆴 Removendo...");
            }
            SwipeRemoveAnim *anim = g_malloc0(sizeof(SwipeRemoveAnim));
            anim->hbox = g_object_ref(sbox);
            anim->idx = app.q_drag_from_idx;
            anim->step = 0;
            int sm = (int)app.q_last_dx;
            if (sm > 180) sm = 180;
            anim->start_margin = sm;
            g_timeout_add(16, animate_swipe_remove_step, anim);
        } else if (sbox) {
            GtkStyleContext *sctx = gtk_widget_get_style_context(sbox);
            gtk_style_context_remove_class(sctx, "swiping-remove-row");
            gtk_style_context_remove_class(sctx, "swiping-remove-armed");
            gtk_widget_set_margin_start(sbox, 0);
            gtk_widget_set_opacity(sbox, 1.0);
            if (app.q_drag_grip_lbl) {
                gtk_label_set_text(GTK_LABEL(app.q_drag_grip_lbl),
                    app.q_drag_from_idx == app.last_queue_idx ? "󰕾" : "󰇙");
            }
        }
        app.q_drag_grip_lbl = NULL;
        return TRUE;
    }

    if (app.q_drag_source_box) {
        gtk_style_context_remove_class(gtk_widget_get_style_context(app.q_drag_source_box), "dragging-row");
        app.q_drag_source_box = NULL;
    }
    app.q_drag_grip_lbl = NULL;

    if (app.q_drag_moved) {
        if (app.q_drag_from_idx != app.q_drag_to_idx && app.q_drag_to_idx >= 0) {
            move_queue_item_to(app.q_drag_from_idx, app.q_drag_to_idx);
        }
    } else {
        play_queue_index_now(app.q_drag_from_idx);
    }
    return TRUE;
}

/* Async Collection (Album/Playlist) Loader */
typedef struct {
    char browse_id[128];
    char col_type[32];
    char title[128];
    JsonNode *result;
} CollectionJob;

static gboolean on_collection_loaded_main(gpointer data) {
    CollectionJob *job = (CollectionJob *)data;
    if (job->result && JSON_NODE_HOLDS_OBJECT(job->result)) {
        JsonObject *obj = json_node_get_object(job->result);
        const char *col_title = j_str(obj, "title", job->title);
        if (json_object_has_member(obj, "tracks")) {
            JsonArray *tracks = json_object_get_array_member(obj, "tracks");
            if (app.current_collection_tracks) json_array_unref(app.current_collection_tracks);
            app.current_collection_tracks = json_array_ref(tracks);

            char header_txt[256];
            snprintf(header_txt, sizeof(header_txt), "📂 %s (%u faixas)", col_title, json_array_get_length(tracks));
            gtk_label_set_text(GTK_LABEL(app.search_collection_lbl), header_txt);
            gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "");
            gtk_widget_show_all(app.search_back_box);

            populate_track_rows(app.search_list, tracks, FALSE);
            gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "search");
        }
    } else {
        gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "Erro ao abrir coleção.");
    }
    if (job->result) json_node_unref(job->result);
    g_free(job);
    return FALSE;
}

static gpointer collection_worker_thread(gpointer data) {
    CollectionJob *job = (CollectionJob *)data;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "get_collection");
    json_builder_set_member_name(b, "browseId");
    json_builder_add_string_value(b, job->browse_id);
    json_builder_set_member_name(b, "collection_type");
    json_builder_add_string_value(b, job->col_type);
    json_builder_end_object(b);
    job->result = send_daemon_request(b);
    g_idle_add(on_collection_loaded_main, job);
    return NULL;
}

static void on_open_collection_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    JsonObject *col_obj = (JsonObject *)user_data;
    if (!col_obj) return;
    CollectionJob *job = g_malloc0(sizeof(CollectionJob));
    strncpy(job->browse_id, j_str(col_obj, "browseId", ""), sizeof(job->browse_id) - 1);
    strncpy(job->col_type, j_str(col_obj, "collection_type", "playlist"), sizeof(job->col_type) - 1);
    strncpy(job->title, j_str(col_obj, "title", "Coleção"), sizeof(job->title) - 1);

    gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "󱑽 Carregando faixas da coleção...");
    gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "search");
    g_thread_new("col-loader", collection_worker_thread, job);
}

static gboolean on_normal_row_button_release(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    (void)w;
    if (ev->type == GDK_BUTTON_RELEASE && ev->button == 1) {
        JsonObject *item = (JsonObject *)user_data;
        if (!item) return FALSE;
        const char *item_type = j_str(item, "item_type", "song");
        if (strcmp(item_type, "collection") == 0) {
            on_open_collection_clicked(NULL, item);
        } else {
            on_row_play_track(NULL, item);
        }
        return TRUE;
    }
    return FALSE;
}

static void populate_track_rows(GtkWidget *listbox, JsonArray *items, gboolean is_queue) {
    clear_container(listbox);
    if (!items) return;

    guint len = json_array_get_length(items);
    for (guint i = 0; i < len; i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        if (!item) continue;

        const char *item_type = j_str(item, "item_type", "song");
        const char *title = j_str(item, "title", "Sem título");
        const char *artist = j_str(item, "artist", "");
        const char *dur = j_str(item, "duration", "");
        const char *cover = j_str(item, "cover_small", "/tmp/waybar-ytmusic-covers/default_44.png");
        const char *exp_cover = j_str(item, "expected_cover_small", "");
        gboolean liked = j_bool(item, "liked", FALSE);

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "track-row");
        if (is_queue && (int)i == app.last_queue_idx) {
            gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "active-queue-row");
        }
        g_object_set_data(G_OBJECT(row), "row-hbox", hbox);

        /* Clickable / Draggable song area */
        GtkWidget *click_evbox = gtk_event_box_new();
        gtk_event_box_set_visible_window(GTK_EVENT_BOX(click_evbox), FALSE);
        gtk_widget_add_events(click_evbox,
            GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK | GDK_POINTER_MOTION_MASK);
        g_object_set_data(G_OBJECT(click_evbox), "row-hbox", hbox);

        GtkWidget *left_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

        if (is_queue) {
            GtkWidget *grip_lbl = gtk_label_new((int)i == app.last_queue_idx ? "󰕾" : "󰇙");
            gtk_style_context_add_class(gtk_widget_get_style_context(grip_lbl), "drag-grip");
            gtk_box_pack_start(GTK_BOX(left_hbox), grip_lbl, FALSE, FALSE, 0);
            g_object_set_data(G_OBJECT(click_evbox), "grip-lbl", grip_lbl);
            gtk_widget_set_tooltip_text(click_evbox,
                "Clique para tocar • Arraste ↑/↓ para mover • Arraste → para remover");
            g_signal_connect(click_evbox, "button-press-event", G_CALLBACK(on_queue_row_button_press), GINT_TO_POINTER((int)i));
            g_signal_connect(click_evbox, "motion-notify-event", G_CALLBACK(on_queue_row_motion), GINT_TO_POINTER((int)i));
            g_signal_connect(click_evbox, "button-release-event", G_CALLBACK(on_queue_row_button_release), GINT_TO_POINTER((int)i));
        } else {
            gtk_widget_set_tooltip_text(click_evbox,
                strcmp(item_type, "collection") == 0 ? "Clique para abrir a coleção" : "Clique na música para tocar agora");
            g_object_set_data_full(G_OBJECT(click_evbox), "json-item", json_object_ref(item), (GDestroyNotify)json_object_unref);
            g_signal_connect(click_evbox, "button-release-event", G_CALLBACK(on_normal_row_button_release), item);
        }

        /* Cover thumbnail */
        GtkWidget *img = gtk_image_new();
        if (exp_cover[0] != '\0' && g_file_test(exp_cover, G_FILE_TEST_EXISTS)) {
            set_image_from_path(GTK_IMAGE(img), exp_cover, 40);
        } else {
            set_image_from_path(GTK_IMAGE(img), cover, 40);
            if (exp_cover[0] != '\0') {
                register_pending_cover(img, exp_cover);
            }
        }
        gtk_box_pack_start(GTK_BOX(left_hbox), img, FALSE, FALSE, 0);

        /* Title & Subtitle */
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *lbl_t = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(lbl_t), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_t), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(lbl_t), 1);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl_t), 36);
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_t), "row-title");

        char sub_line[256];
        if (dur && dur[0]) {
            snprintf(sub_line, sizeof(sub_line), "%s • %s", artist, dur);
        } else {
            snprintf(sub_line, sizeof(sub_line), "%s", artist);
        }
        GtkWidget *lbl_s = gtk_label_new(sub_line);
        gtk_label_set_xalign(GTK_LABEL(lbl_s), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_s), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(lbl_s), 1);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl_s), 40);
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_s), "row-sub");

        gtk_box_pack_start(GTK_BOX(vbox), lbl_t, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), lbl_s, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(left_hbox), vbox, TRUE, TRUE, 0);

        gtk_container_add(GTK_CONTAINER(click_evbox), left_hbox);
        gtk_box_pack_start(GTK_BOX(hbox), click_evbox, TRUE, TRUE, 0);

        /* Action Buttons (No redundant play button; clicking the song row plays it!) */
        if (strcmp(item_type, "collection") == 0) {
            GtkWidget *btn_open = gtk_button_new_with_label("📂 Abrir");
            gtk_style_context_add_class(gtk_widget_get_style_context(btn_open), "row-btn");
            g_object_set_data_full(G_OBJECT(btn_open), "json-item", json_object_ref(item), (GDestroyNotify)json_object_unref);
            g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_collection_clicked), item);
            gtk_box_pack_end(GTK_BOX(hbox), btn_open, FALSE, FALSE, 0);
        } else if (!is_queue) {
            GtkWidget *btn_like = gtk_button_new_with_label(liked ? "♥" : "♡");
            gtk_style_context_add_class(gtk_widget_get_style_context(btn_like), "row-btn");
            if (liked) gtk_style_context_add_class(gtk_widget_get_style_context(btn_like), "liked-btn");
            g_object_set_data_full(G_OBJECT(btn_like), "json-item", json_object_ref(item), (GDestroyNotify)json_object_unref);
            g_signal_connect(btn_like, "clicked", G_CALLBACK(on_row_toggle_like), item);

            GtkWidget *btn_q = gtk_button_new_with_label("+Fila");
            gtk_style_context_add_class(gtk_widget_get_style_context(btn_q), "row-btn");
            g_object_set_data_full(G_OBJECT(btn_q), "json-item", json_object_ref(item), (GDestroyNotify)json_object_unref);
            g_signal_connect(btn_q, "clicked", G_CALLBACK(on_row_add_queue), item);

            gtk_box_pack_end(GTK_BOX(hbox), btn_like, FALSE, FALSE, 0);
            gtk_box_pack_end(GTK_BOX(hbox), btn_q, FALSE, FALSE, 0);
        }

        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_container_add(GTK_CONTAINER(listbox), row);
    }
    gtk_widget_show_all(listbox);
}

/* --- Home Tab --- */
static void on_play_home_section_clicked(GtkButton *btn, gpointer user_data) {
    gboolean shuf = GPOINTER_TO_INT(user_data) != 0;
    JsonArray *songs = (JsonArray *)g_object_get_data(G_OBJECT(btn), "section-songs");
    if (!songs || json_array_get_length(songs) == 0) return;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "play_tracks_list");
    json_builder_set_member_name(b, "shuffle");
    json_builder_add_boolean_value(b, shuf);
    json_builder_set_member_name(b, "tracks");
    JsonNode *acopy = json_node_new(JSON_NODE_ARRAY);
    json_node_set_array(acopy, songs);
    json_builder_add_value(b, acopy);
    json_builder_end_object(b);

    JsonNode *res = send_daemon_request(b);
    if (res) {
        if (JSON_NODE_HOLDS_OBJECT(res)) refresh_status_ui(json_node_get_object(res));
        json_node_unref(res);
    }
}

static gboolean on_home_loaded_main(gpointer data) {
    JsonNode *res = (JsonNode *)data;
    clear_container(app.home_sections_box);

    if (res && JSON_NODE_HOLDS_OBJECT(res)) {
        JsonObject *obj = json_node_get_object(res);
        if (json_object_has_member(obj, "sections")) {
            JsonArray *secs = json_object_get_array_member(obj, "sections");
            guint slen = json_array_get_length(secs);
            gtk_label_set_text(GTK_LABEL(app.home_status_lbl),
                               slen > 0 ? "Para você • Recomendações e mais ouvidas" : "Nenhuma seção disponível");

            for (guint s = 0; s < slen; s++) {
                JsonObject *sec = json_array_get_object_element(secs, s);
                if (!sec) continue;
                const char *stitle = j_str(sec, "title", "Recomendados");
                if (!json_object_has_member(sec, "items")) continue;
                JsonArray *items = json_object_get_array_member(sec, "items");
                if (!items || json_array_get_length(items) == 0) continue;

                /* Collect song items in this section for quick "Tocar Tudo / Mix" */
                JsonArray *song_items = json_array_new();
                for (guint k = 0; k < json_array_get_length(items); k++) {
                    JsonObject *it = json_array_get_object_element(items, k);
                    if (it && strcmp(j_str(it, "item_type", "song"), "song") == 0) {
                        json_array_add_object_element(song_items, json_object_ref(it));
                    }
                }

                GtkWidget *sec_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                GtkWidget *lbl = gtk_label_new(stitle);
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
                gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "section-lbl");
                gtk_box_pack_start(GTK_BOX(sec_hdr), lbl, TRUE, TRUE, 0);

                if (json_array_get_length(song_items) > 1) {
                    GtkWidget *btn_mix = gtk_button_new_with_label("󰒝 Mix");
                    gtk_style_context_add_class(gtk_widget_get_style_context(btn_mix), "row-btn");
                    g_object_set_data_full(G_OBJECT(btn_mix), "section-songs",
                                           json_array_ref(song_items), (GDestroyNotify)json_array_unref);
                    g_signal_connect(btn_mix, "clicked", G_CALLBACK(on_play_home_section_clicked), GINT_TO_POINTER(1));

                    GtkWidget *btn_play_sec = gtk_button_new_with_label("󰐊 Tocar");
                    gtk_style_context_add_class(gtk_widget_get_style_context(btn_play_sec), "row-btn-primary");
                    g_object_set_data_full(G_OBJECT(btn_play_sec), "section-songs",
                                           json_array_ref(song_items), (GDestroyNotify)json_array_unref);
                    g_signal_connect(btn_play_sec, "clicked", G_CALLBACK(on_play_home_section_clicked), GINT_TO_POINTER(0));

                    gtk_box_pack_end(GTK_BOX(sec_hdr), btn_mix, FALSE, FALSE, 0);
                    gtk_box_pack_end(GTK_BOX(sec_hdr), btn_play_sec, FALSE, FALSE, 0);
                }
                json_array_unref(song_items);

                GtkWidget *sec_list = gtk_list_box_new();
                gtk_list_box_set_selection_mode(GTK_LIST_BOX(sec_list), GTK_SELECTION_NONE);
                populate_track_rows(sec_list, items, FALSE);

                gtk_box_pack_start(GTK_BOX(app.home_sections_box), sec_hdr, FALSE, FALSE, 2);
                gtk_box_pack_start(GTK_BOX(app.home_sections_box), sec_list, FALSE, FALSE, 4);
            }
            gtk_widget_show_all(app.home_sections_box);
        }
    } else {
        gtk_label_set_text(GTK_LABEL(app.home_status_lbl), "Erro ao carregar recomendações.");
    }
    if (res) json_node_unref(res);
    return FALSE;
}

static gpointer home_worker_thread(gpointer data) {
    gboolean force = GPOINTER_TO_INT(data) != 0;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "get_home");
    json_builder_set_member_name(b, "refresh");
    json_builder_add_boolean_value(b, force);
    json_builder_end_object(b);
    JsonNode *res = send_daemon_request(b);
    g_idle_add(on_home_loaded_main, res);
    return NULL;
}

static void refresh_home_tab(gboolean force_refresh) {
    gtk_label_set_text(GTK_LABEL(app.home_status_lbl), "󱑽 Carregando recomendações e mais ouvidas...");
    g_thread_new("home-worker", home_worker_thread, GINT_TO_POINTER(force_refresh ? 1 : 0));
}

static void on_refresh_home_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    refresh_home_tab(TRUE);
}

/* --- Search Tab --- */
typedef struct {
    char query[256];
    char filter[32];
    gboolean save_history;
    guint seq;
    JsonNode *result;
} SearchJob;

static void trigger_search(void);
static void trigger_search_ext(gboolean save_hist);

static void on_suggestion_pill_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    const char *sug = (const char *)g_object_get_data(G_OBJECT(btn), "sug-text");
    if (!sug || !sug[0]) return;
    if (app.search_debounce_id > 0) {
        g_source_remove(app.search_debounce_id);
        app.search_debounce_id = 0;
    }
    app.suppress_search_changed = TRUE;
    gtk_entry_set_text(GTK_ENTRY(app.search_entry), sug);
    gtk_editable_set_position(GTK_EDITABLE(app.search_entry), -1);
    app.suppress_search_changed = FALSE;
    trigger_search_ext(TRUE);
}

static void on_remove_search_hist_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    const char *q = (const char *)g_object_get_data(G_OBJECT(btn), "hist-text");
    if (!q || !q[0]) return;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "remove_search_history");
    json_builder_set_member_name(b, "query");
    json_builder_add_string_value(b, q);
    json_builder_end_object(b);
    JsonNode *res = send_daemon_request(b);
    if (res) json_node_unref(res);
    refresh_search_history_ui();
}

static void on_clear_search_hist_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    JsonNode *res = send_simple_cmd("clear_search_history");
    if (res) json_node_unref(res);
    refresh_search_history_ui();
}

static void refresh_search_history_ui(void) {
    const char *cur_q = gtk_entry_get_text(GTK_ENTRY(app.search_entry));
    if (cur_q && cur_q[0] != '\0') return;

    gtk_widget_hide(app.search_back_box);
    clear_container(app.search_suggestions_box);
    clear_container(app.search_list);

    JsonNode *res = send_simple_cmd("get_search_history");
    if (!res || !JSON_NODE_HOLDS_OBJECT(res)) {
        if (res) json_node_unref(res);
        gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "Digite acima para buscar músicas");
        gtk_widget_hide(app.btn_clear_search_hist);
        return;
    }

    JsonObject *obj = json_node_get_object(res);
    if (json_object_has_member(obj, "history")) {
        JsonArray *hist = json_object_get_array_member(obj, "history");
        guint hlen = json_array_get_length(hist);
        if (hlen > 0) {
            gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "🕒 Últimas buscas");
            gtk_widget_show(app.btn_clear_search_hist);

            /* Top quick chips */
            for (guint i = 0; i < hlen && i < 4; i++) {
                const char *htxt = json_array_get_string_element(hist, i);
                if (!htxt || !htxt[0]) continue;
                char chip_lbl[64];
                snprintf(chip_lbl, sizeof(chip_lbl), "󰋚 %.20s%s", htxt, strlen(htxt) > 20 ? "…" : "");
                GtkWidget *cbtn = gtk_button_new_with_label(chip_lbl);
                gtk_widget_set_tooltip_text(cbtn, htxt);
                gtk_style_context_add_class(gtk_widget_get_style_context(cbtn), "row-btn");
                g_object_set_data_full(G_OBJECT(cbtn), "sug-text", g_strdup(htxt), g_free);
                g_signal_connect(cbtn, "clicked", G_CALLBACK(on_suggestion_pill_clicked), NULL);
                gtk_box_pack_start(GTK_BOX(app.search_suggestions_box), cbtn, FALSE, FALSE, 0);
            }
            gtk_widget_show_all(app.search_suggestions_box);

            /* Full history list rows */
            for (guint i = 0; i < hlen; i++) {
                const char *htxt = json_array_get_string_element(hist, i);
                if (!htxt || !htxt[0]) continue;

                GtkWidget *row = gtk_list_box_row_new();
                GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "track-row");

                GtkWidget *ic = gtk_label_new("󰋚");
                gtk_style_context_add_class(gtk_widget_get_style_context(ic), "section-lbl");
                gtk_box_pack_start(GTK_BOX(hbox), ic, FALSE, FALSE, 4);

                GtkWidget *lbl = gtk_label_new(htxt);
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
                gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
                gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "row-title");
                gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);

                GtkWidget *btn_del = gtk_button_new_with_label("✕");
                gtk_widget_set_tooltip_text(btn_del, "Remover do histórico de buscas");
                gtk_style_context_add_class(gtk_widget_get_style_context(btn_del), "row-btn");
                g_object_set_data_full(G_OBJECT(btn_del), "hist-text", g_strdup(htxt), g_free);
                g_signal_connect(btn_del, "clicked", G_CALLBACK(on_remove_search_hist_clicked), NULL);

                GtkWidget *btn_go = gtk_button_new_with_label("󰍉 Buscar");
                gtk_style_context_add_class(gtk_widget_get_style_context(btn_go), "row-btn-primary");
                g_object_set_data_full(G_OBJECT(btn_go), "sug-text", g_strdup(htxt), g_free);
                g_signal_connect(btn_go, "clicked", G_CALLBACK(on_suggestion_pill_clicked), NULL);

                gtk_box_pack_end(GTK_BOX(hbox), btn_del, FALSE, FALSE, 0);
                gtk_box_pack_end(GTK_BOX(hbox), btn_go, FALSE, FALSE, 0);

                gtk_container_add(GTK_CONTAINER(row), hbox);
                gtk_container_add(GTK_CONTAINER(app.search_list), row);
            }
            gtk_widget_show_all(app.search_list);
        } else {
            gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "Sem buscas recentes • Digite acima para buscar");
            gtk_widget_hide(app.btn_clear_search_hist);
            gtk_widget_hide(app.search_suggestions_box);
        }
    }
    json_node_unref(res);
}

static gboolean on_search_done_main(gpointer data) {
    SearchJob *job = (SearchJob *)data;
    if (job->seq != app.search_seq) {
        if (job->result) json_node_unref(job->result);
        g_free(job);
        return FALSE;
    }

    gtk_widget_hide(app.search_back_box);
    gtk_widget_hide(app.btn_clear_search_hist);
    clear_container(app.search_suggestions_box);

    if (job->result && JSON_NODE_HOLDS_OBJECT(job->result)) {
        JsonObject *obj = json_node_get_object(job->result);
        guint added_pills = 0;

        /* First show matching search history pills if any */
        if (json_object_has_member(obj, "search_history")) {
            JsonArray *hist = json_object_get_array_member(obj, "search_history");
            guint hlen = json_array_get_length(hist);
            for (guint i = 0; i < hlen && added_pills < 2; i++) {
                const char *htxt = json_array_get_string_element(hist, i);
                if (!htxt || !htxt[0]) continue;
                if (g_ascii_strcasecmp(htxt, job->query) == 0) continue;
                char *h_low = g_ascii_strdown(htxt, -1);
                char *q_low = g_ascii_strdown(job->query, -1);
                gboolean match = (strstr(h_low, q_low) != NULL);
                g_free(h_low);
                g_free(q_low);
                if (!match) continue;

                char btn_lbl[64];
                snprintf(btn_lbl, sizeof(btn_lbl), "󰋚 %.22s%s", htxt, strlen(htxt) > 22 ? "…" : "");
                GtkWidget *hbtn = gtk_button_new_with_label(btn_lbl);
                gtk_widget_set_tooltip_text(hbtn, htxt);
                gtk_style_context_add_class(gtk_widget_get_style_context(hbtn), "row-btn");
                g_object_set_data_full(G_OBJECT(hbtn), "sug-text", g_strdup(htxt), g_free);
                g_signal_connect(hbtn, "clicked", G_CALLBACK(on_suggestion_pill_clicked), NULL);
                gtk_box_pack_start(GTK_BOX(app.search_suggestions_box), hbtn, FALSE, FALSE, 0);
                added_pills++;
            }
        }

        /* Render suggestion pills if available */
        if (json_object_has_member(obj, "suggestions")) {
            JsonArray *sugs = json_object_get_array_member(obj, "suggestions");
            guint scount = json_array_get_length(sugs);
            for (guint i = 0; i < scount && added_pills < 4; i++) {
                const char *stxt = json_array_get_string_element(sugs, i);
                if (!stxt || !stxt[0]) continue;
                char btn_lbl[64];
                snprintf(btn_lbl, sizeof(btn_lbl), "󰍉 %.22s%s", stxt, strlen(stxt) > 22 ? "…" : "");
                GtkWidget *sbtn = gtk_button_new_with_label(btn_lbl);
                gtk_widget_set_tooltip_text(sbtn, stxt);
                gtk_style_context_add_class(gtk_widget_get_style_context(sbtn), "row-btn");
                g_object_set_data_full(G_OBJECT(sbtn), "sug-text", g_strdup(stxt), g_free);
                g_signal_connect(sbtn, "clicked", G_CALLBACK(on_suggestion_pill_clicked), NULL);
                gtk_box_pack_start(GTK_BOX(app.search_suggestions_box), sbtn, FALSE, FALSE, 0);
                added_pills++;
            }
        }

        if (added_pills > 0) {
            gtk_widget_show_all(app.search_suggestions_box);
        } else {
            gtk_widget_hide(app.search_suggestions_box);
        }

        if (json_object_has_member(obj, "items")) {
            JsonArray *arr = json_object_get_array_member(obj, "items");
            guint count = json_array_get_length(arr);
            char msg[128];
            snprintf(msg, sizeof(msg), "%u resultados", count);
            gtk_label_set_text(GTK_LABEL(app.search_status_lbl), msg);
            populate_track_rows(app.search_list, arr, FALSE);
        } else {
            gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "Nenhum resultado encontrado.");
        }
    } else {
        gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "Erro ao realizar busca.");
    }
    if (job->result) json_node_unref(job->result);
    g_free(job);
    return FALSE;
}

static gpointer search_worker_thread(gpointer data) {
    SearchJob *job = (SearchJob *)data;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "search");
    json_builder_set_member_name(b, "query");
    json_builder_add_string_value(b, job->query);
    json_builder_set_member_name(b, "filter");
    json_builder_add_string_value(b, job->filter);
    json_builder_set_member_name(b, "save_history");
    json_builder_add_boolean_value(b, job->save_history);
    json_builder_end_object(b);
    job->result = send_daemon_request(b);
    g_idle_add(on_search_done_main, job);
    return NULL;
}

static void trigger_search_ext(gboolean save_hist) {
    if (app.search_debounce_id > 0) {
        g_source_remove(app.search_debounce_id);
        app.search_debounce_id = 0;
    }
    const char *q = gtk_entry_get_text(GTK_ENTRY(app.search_entry));
    if (!q || q[0] == '\0') {
        refresh_search_history_ui();
        return;
    }

    app.search_seq++;
    gtk_label_set_text(GTK_LABEL(app.search_status_lbl), "󱑽 Buscando...");
    SearchJob *job = g_malloc0(sizeof(SearchJob));
    strncpy(job->query, q, sizeof(job->query) - 1);
    memcpy(job->filter, app.current_filter, sizeof(job->filter));
    job->save_history = save_hist;
    job->seq = app.search_seq;
    g_thread_new("search-worker", search_worker_thread, job);
}

static void trigger_search(void) {
    trigger_search_ext(TRUE);
}

static gboolean on_search_debounce_timeout(gpointer data) {
    (void)data;
    app.search_debounce_id = 0;
    trigger_search_ext(TRUE);
    return FALSE;
}

static void on_search_entry_changed(GtkEditable *editable, gpointer data) {
    (void)data;
    if (app.suppress_search_changed) return;
    if (app.search_debounce_id > 0) {
        g_source_remove(app.search_debounce_id);
        app.search_debounce_id = 0;
    }
    const char *q = gtk_entry_get_text(GTK_ENTRY(editable));
    if (!q || q[0] == '\0') {
        refresh_search_history_ui();
        return;
    }
    if (strlen(q) < 2) {
        clear_container(app.search_suggestions_box);
        gtk_widget_hide(app.search_suggestions_box);
        return;
    }
    app.search_debounce_id = g_timeout_add(340, on_search_debounce_timeout, NULL);
}

static void update_filter_buttons_style(void) {
    struct { GtkWidget *btn; const char *f; } map[3] = {
        {app.filter_songs_btn, "songs"},
        {app.filter_albums_btn, "albums"},
        {app.filter_playlists_btn, "playlists"},
    };
    for (int i = 0; i < 3; i++) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(map[i].btn);
        if (strcmp(app.current_filter, map[i].f) == 0) {
            gtk_style_context_add_class(ctx, "toggle-on");
        } else {
            gtk_style_context_remove_class(ctx, "toggle-on");
        }
    }
}

static void on_filter_btn_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *f = (const char *)user_data;
    strncpy(app.current_filter, f, sizeof(app.current_filter) - 1);
    update_filter_buttons_style();
    trigger_search();
}

static void on_play_collection_all_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    gboolean shuf = GPOINTER_TO_INT(user_data) != 0;
    if (!app.current_collection_tracks) return;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "play_tracks_list");
    json_builder_set_member_name(b, "shuffle");
    json_builder_add_boolean_value(b, shuf);
    json_builder_set_member_name(b, "tracks");
    JsonNode *acopy = json_node_new(JSON_NODE_ARRAY);
    json_node_set_array(acopy, app.current_collection_tracks);
    json_builder_add_value(b, acopy);
    json_builder_end_object(b);

    JsonNode *res = send_daemon_request(b);
    if (res) {
        if (JSON_NODE_HOLDS_OBJECT(res)) refresh_status_ui(json_node_get_object(res));
        json_node_unref(res);
    }
}

/* --- Queue Tab --- */
static void refresh_queue_tab(void) {
    JsonNode *res = send_simple_cmd("get_queue");
    if (!res || !JSON_NODE_HOLDS_OBJECT(res)) {
        if (res) json_node_unref(res);
        return;
    }
    JsonObject *obj = json_node_get_object(res);
    app.last_queue_idx = (int)j_int(obj, "queue_index", -1);
    if (json_object_has_member(obj, "queue")) {
        JsonArray *q = json_object_get_array_member(obj, "queue");
        guint len = json_array_get_length(q);
        char buf[128];
        snprintf(buf, sizeof(buf), "Fila de Reprodução (%u faixas)", len);
        gtk_label_set_text(GTK_LABEL(app.queue_count_lbl), buf);
        populate_track_rows(app.queue_list, q, TRUE);
    }
    json_node_unref(res);
}

static void on_clear_queue_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *res = send_simple_cmd("clear_queue");
    if (res) {
        if (JSON_NODE_HOLDS_OBJECT(res)) refresh_status_ui(json_node_get_object(res));
        json_node_unref(res);
    }
    refresh_queue_tab();
}

/* --- Liked Songs Tab --- */
static gboolean on_liked_loaded_main(gpointer data) {
    JsonNode *res = (JsonNode *)data;
    if (res && JSON_NODE_HOLDS_OBJECT(res)) {
        JsonObject *obj = json_node_get_object(res);
        if (json_object_has_member(obj, "tracks")) {
            JsonArray *arr = json_object_get_array_member(obj, "tracks");
            if (app.liked_tracks_cache) json_array_unref(app.liked_tracks_cache);
            app.liked_tracks_cache = json_array_ref(arr);
            char msg[128];
            snprintf(msg, sizeof(msg), "♥ %u músicas curtidas", json_array_get_length(arr));
            gtk_label_set_text(GTK_LABEL(app.liked_status_lbl), msg);
            populate_track_rows(app.liked_list, arr, FALSE);
        }
    }
    if (res) json_node_unref(res);
    return FALSE;
}

static gpointer liked_worker_thread(gpointer data) {
    (void)data;
    JsonNode *res = send_simple_cmd("get_liked");
    g_idle_add(on_liked_loaded_main, res);
    return NULL;
}

static void refresh_liked_tab(void) {
    gtk_label_set_text(GTK_LABEL(app.liked_status_lbl), "󱑽 Carregando músicas curtidas...");
    g_thread_new("liked-worker", liked_worker_thread, NULL);
}

static void on_play_all_liked_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    gboolean shuf = GPOINTER_TO_INT(user_data) != 0;
    if (!app.liked_tracks_cache || json_array_get_length(app.liked_tracks_cache) == 0) return;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "play_tracks_list");
    json_builder_set_member_name(b, "shuffle");
    json_builder_add_boolean_value(b, shuf);
    json_builder_set_member_name(b, "tracks");
    JsonNode *acopy = json_node_new(JSON_NODE_ARRAY);
    json_node_set_array(acopy, app.liked_tracks_cache);
    json_builder_add_value(b, acopy);
    json_builder_end_object(b);

    JsonNode *res = send_daemon_request(b);
    if (res) {
        if (JSON_NODE_HOLDS_OBJECT(res)) refresh_status_ui(json_node_get_object(res));
        json_node_unref(res);
    }
}

/* --- Library Tab --- */
static gboolean on_library_loaded_main(gpointer data) {
    JsonNode *res = (JsonNode *)data;
    if (res && JSON_NODE_HOLDS_OBJECT(res)) {
        JsonObject *obj = json_node_get_object(res);
        const char *src = j_str(obj, "auth_source", "");
        gtk_label_set_text(GTK_LABEL(app.lib_status_lbl), src);
        if (json_object_has_member(obj, "playlists")) {
            populate_track_rows(app.lib_playlists_list, json_object_get_array_member(obj, "playlists"), FALSE);
        }
        if (json_object_has_member(obj, "history")) {
            populate_track_rows(app.lib_history_list, json_object_get_array_member(obj, "history"), FALSE);
        }
    }
    if (res) json_node_unref(res);
    return FALSE;
}

static gpointer library_worker_thread(gpointer data) {
    (void)data;
    JsonNode *res = send_simple_cmd("get_library");
    g_idle_add(on_library_loaded_main, res);
    return NULL;
}

static void refresh_library_tab(void) {
    gtk_label_set_text(GTK_LABEL(app.lib_status_lbl), "󱑽 Carregando sua biblioteca e histórico...");
    g_thread_new("lib-worker", library_worker_thread, NULL);
}

/* --- Account Tab Callbacks --- */
typedef struct {
    char browser[32];
    JsonNode *result;
} AuthJob;

static gboolean on_auth_done_main(gpointer data) {
    AuthJob *job = (AuthJob *)data;
    if (job->result && JSON_NODE_HOLDS_OBJECT(job->result)) {
        JsonObject *obj = json_node_get_object(job->result);
        const char *msg = j_str(obj, "message", "Concluído.");
        gtk_label_set_text(GTK_LABEL(app.auth_msg_lbl), msg);
    } else {
        gtk_label_set_text(GTK_LABEL(app.auth_msg_lbl), "Erro ao comunicar com o daemon.");
    }
    if (job->result) json_node_unref(job->result);
    g_free(job);
    poll_and_update();
    refresh_library_tab();
    refresh_liked_tab();
    return FALSE;
}

static gpointer auth_worker_thread(gpointer data) {
    AuthJob *job = (AuthJob *)data;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "import_browser_auth");
    json_builder_set_member_name(b, "browser");
    json_builder_add_string_value(b, job->browser);
    json_builder_end_object(b);
    job->result = send_daemon_request(b);
    g_idle_add(on_auth_done_main, job);
    return NULL;
}

static void on_import_browser_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *browser = (const char *)user_data;
    gtk_label_set_text(GTK_LABEL(app.auth_msg_lbl), "󱑽 Importando cookies do navegador...");
    AuthJob *job = g_malloc0(sizeof(AuthJob));
    strncpy(job->browser, browser, sizeof(job->browser) - 1);
    g_thread_new("auth-worker", auth_worker_thread, job);
}

static void on_save_headers_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.headers_textview));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, "setup_raw_headers");
    json_builder_set_member_name(b, "headers");
    json_builder_add_string_value(b, text ? text : "");
    json_builder_end_object(b);
    g_free(text);

    JsonNode *res = send_daemon_request(b);
    if (res && JSON_NODE_HOLDS_OBJECT(res)) {
        const char *msg = j_str(json_node_get_object(res), "message", "Atualizado.");
        gtk_label_set_text(GTK_LABEL(app.auth_msg_lbl), msg);
    }
    if (res) json_node_unref(res);
    poll_and_update();
}

static void on_logout_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    JsonNode *res = send_simple_cmd("logout_auth");
    if (res && JSON_NODE_HOLDS_OBJECT(res)) {
        gtk_label_set_text(GTK_LABEL(app.auth_msg_lbl), j_str(json_node_get_object(res), "message", "Desconectado."));
    }
    if (res) json_node_unref(res);
    poll_and_update();
}

static void style_switcher_btn_cb(GtkWidget *w, gpointer data) {
    (void)data;
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    if (GTK_IS_TOGGLE_BUTTON(w) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w))) {
        gtk_style_context_add_class(ctx, "active-nav-tab");
        gtk_style_context_remove_class(ctx, "inactive-nav-tab");
    } else {
        gtk_style_context_remove_class(ctx, "active-nav-tab");
        gtk_style_context_add_class(ctx, "inactive-nav-tab");
    }
}

static void update_switcher_active_style(void) {
    if (app.switcher && GTK_IS_CONTAINER(app.switcher)) {
        gtk_container_foreach(GTK_CONTAINER(app.switcher), style_switcher_btn_cb, NULL);
    }
}

static void set_expanded_mode(gboolean expand) {
    app.expanded = expand;
    if (expand) {
        gtk_button_set_label(GTK_BUTTON(app.btn_expand), "󰁅 Recolher");
        gtk_widget_set_tooltip_text(app.btn_expand, "Recolher para o Mini-Player compacto");
        gtk_widget_set_size_request(app.cover_img, 84, 84);
        if (app.current_cover_path[0]) {
            set_image_from_path(GTK_IMAGE(app.cover_img), app.current_cover_path, 84);
        }
        gtk_widget_show(app.expanded_box);
        gtk_widget_set_size_request(app.main_box, 540, 620);

        gboolean has_active_music = (app.current_track_obj != NULL) &&
            (strcmp(app.current_state, "playing") == 0 ||
             strcmp(app.current_state, "loading") == 0 ||
             strcmp(app.current_state, "paused") == 0);

        if (has_active_music) {
            gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "queue");
            refresh_queue_tab();
        } else {
            if (!app.home_loaded_once) {
                app.home_loaded_once = TRUE;
                refresh_home_tab(FALSE);
            }
            refresh_queue_tab();
        }
        update_switcher_active_style();
    } else {
        gtk_button_set_label(GTK_BUTTON(app.btn_expand), "󰁝 Expandir");
        gtk_widget_set_tooltip_text(app.btn_expand, "Expandir player completo (Início, Buscar, Fila...)");
        gtk_widget_set_size_request(app.cover_img, 56, 56);
        if (app.current_cover_path[0]) {
            set_image_from_path(GTK_IMAGE(app.cover_img), app.current_cover_path, 56);
        }
        gtk_widget_hide(app.expanded_box);
        gtk_widget_set_size_request(app.main_box, 510, -1);
    }
}

static void on_expand_btn_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    set_expanded_mode(!app.expanded);
}

static gboolean on_np_card_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w; (void)data;
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 1) {
        set_expanded_mode(!app.expanded);
        return TRUE;
    }
    return FALSE;
}

static void on_stack_visible_child_changed(GObject *stack_obj, GParamSpec *pspec, gpointer data) {
    (void)pspec; (void)data;
    update_switcher_active_style();
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack_obj));
    if (!name) return;
    if (strcmp(name, "home") == 0) {
        app.home_loaded_once = TRUE;
        refresh_home_tab(FALSE);
    } else if (strcmp(name, "search") == 0) {
        const char *q = gtk_entry_get_text(GTK_ENTRY(app.search_entry));
        if (!q || q[0] == '\0') {
            refresh_search_history_ui();
        }
    } else if (strcmp(name, "queue") == 0) {
        refresh_queue_tab();
    } else if (strcmp(name, "liked") == 0) {
        refresh_liked_tab();
    } else if (strcmp(name, "library") == 0) {
        refresh_library_tab();
    }
}

static gboolean on_poll_timer(gpointer data) {
    (void)data;
    poll_and_update();
    return TRUE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    (void)data;
    if (event->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(widget);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_AudioPlay || event->keyval == GDK_KEY_AudioPause || event->keyval == GDK_KEY_AudioStop) {
        on_play_clicked(NULL, NULL);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_AudioNext) {
        on_next_clicked(NULL, NULL);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_AudioPrev) {
        on_prev_clicked(NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_card_guard_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    (void)widget; (void)ev; (void)data;
    return TRUE;
}

static gboolean on_window_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    (void)data;
    if (ev->type == GDK_BUTTON_PRESS && app.main_box) {
        GtkAllocation alloc;
        gtk_widget_get_allocation(app.main_box, &alloc);
        if (ev->window == gtk_widget_get_window(widget)) {
            if (ev->x < alloc.x || ev->x > (alloc.x + alloc.width) ||
                ev->y < alloc.y || ev->y > (alloc.y + alloc.height)) {
                gtk_widget_destroy(widget);
                return TRUE;
            }
        }
    }
    return FALSE;
}

typedef struct {
    const char *id;
    const char *label;
    const char *bg_main;
    const char *bg_card;
    const char *bg_btn;
    const char *text_main;
    const char *text_sub;
    const char *accent;
    const char *accent_hover;
    const char *accent_rgba;
    const char *artist_color;
} ColorTheme;

#define NUM_THEMES 8
static const ColorTheme THEMES[NUM_THEMES] = {
    {
        "mocha", "🌊 Catppuccin Ciano",
        "rgba(17, 17, 27, 0.96)", "rgba(24, 24, 37, 0.92)", "rgba(49, 50, 68, 0.85)",
        "#cdd6f4", "#a6adc8", "#74c7ec", "#89dceb", "116, 199, 236", "#89b4fa"
    },
    {
        "ytmusic", "🔴 YouTube Music",
        "rgba(15, 15, 15, 0.97)", "rgba(29, 29, 29, 0.94)", "rgba(45, 45, 45, 0.90)",
        "#ffffff", "#aaaaaa", "#ff4e45", "#ff6e66", "255, 78, 69", "#ff8a84"
    },
    {
        "mauve", "🟣 Catppuccin Roxo",
        "rgba(17, 17, 27, 0.96)", "rgba(24, 24, 37, 0.92)", "rgba(49, 50, 68, 0.85)",
        "#cdd6f4", "#bac2de", "#cba6f7", "#d8b9ff", "203, 166, 247", "#b4befe"
    },
    {
        "tokyonight", "🌃 Tokyo Night",
        "rgba(22, 22, 30, 0.96)", "rgba(26, 27, 38, 0.94)", "rgba(41, 46, 66, 0.88)",
        "#c0caf5", "#9aa5ce", "#7aa2f7", "#89ddff", "122, 162, 247", "#bb9af7"
    },
    {
        "dracula", "🧛 Dracula Rosa",
        "rgba(30, 31, 41, 0.96)", "rgba(40, 42, 54, 0.94)", "rgba(68, 71, 90, 0.88)",
        "#f8f8f2", "#bfbfbf", "#ff79c6", "#ff92d0", "255, 121, 198", "#bd93f9"
    },
    {
        "emerald", "🟢 Emerald / Spotify",
        "rgba(13, 17, 23, 0.96)", "rgba(22, 27, 34, 0.94)", "rgba(38, 44, 54, 0.88)",
        "#e6edf3", "#9198a1", "#1ed760", "#3be477", "30, 215, 96", "#58a6ff"
    },
    {
        "gruvbox", "🟠 Gruvbox Âmbar",
        "rgba(29, 32, 33, 0.97)", "rgba(40, 40, 40, 0.94)", "rgba(60, 56, 54, 0.90)",
        "#ebdbb2", "#bdae93", "#fabd2f", "#fe8019", "250, 189, 47", "#83a598"
    },
    {
        "nord", "❄️ Nord Ártico",
        "rgba(36, 41, 51, 0.96)", "rgba(46, 52, 64, 0.94)", "rgba(67, 76, 94, 0.88)",
        "#eceff4", "#d8dee9", "#88c0d0", "#8fbcbb", "136, 192, 208", "#81a1c1"
    }
};

static void get_theme_file_path(char *buf, size_t sz) {
    const char *home = g_get_home_dir();
    snprintf(buf, sz, "%s/.config/waybar-ytmusic/theme.json", home ? home : "/tmp");
}

static void update_theme_buttons_ui(void) {
    for (int i = 0; i < NUM_THEMES; i++) {
        if (!app.theme_btns[i]) continue;
        GtkStyleContext *ctx = gtk_widget_get_style_context(app.theme_btns[i]);
        if (strcmp(app.current_theme_id, THEMES[i].id) == 0) {
            gtk_style_context_add_class(ctx, "active-nav-tab");
        } else {
            gtk_style_context_remove_class(ctx, "active-nav-tab");
        }
    }
}

static void apply_theme_palette(
    const char *bg_main,
    const char *bg_card,
    const char *bg_btn,
    const char *text_main,
    const char *text_sub,
    const char *accent,
    const char *accent_hover,
    const char *accent_rgba,
    const char *artist_color
) {
    if (!app.css_provider) {
        app.css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(app.css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    }

    char css[8192];
    snprintf(css, sizeof(css),
        "window {"
        "  background-color: transparent;"
        "  color: %s;"
        "  border: none;"
        "  box-shadow: none;"
        "  font-family: 'JetBrainsMono Nerd Font', sans-serif;"
        "}"
        ".main-box {"
        "  background-color: %s;"
        "  color: %s;"
        "  border: 2px solid %s;"
        "  border-radius: 14px;"
        "  padding: 14px;"
        "}"
        ".player-card {"
        "  background-color: %s;"
        "  border: 1px solid rgba(%s, 0.30);"
        "  border-radius: 12px;"
        "  padding: 12px;"
        "}"
        ".song-title {"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "  color: %s;"
        "}"
        ".song-artist {"
        "  font-size: 12px;"
        "  color: %s;"
        "  font-weight: bold;"
        "}"
        ".song-album, .time-lbl, .row-sub {"
        "  font-size: 11px;"
        "  color: %s;"
        "}"
        "button {"
        "  background-image: none;"
        "  background-color: %s;"
        "  color: %s;"
        "  border: 1px solid rgba(%s, 0.24);"
        "  border-radius: 8px;"
        "  padding: 5px 10px;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "}"
        "button:hover {"
        "  background-color: %s;"
        "  color: #11111b;"
        "}"
        "stackswitcher {"
        "  background-color: %s;"
        "  border: 1px solid rgba(%s, 0.24);"
        "  border-radius: 10px;"
        "  padding: 3px;"
        "}"
        "stackswitcher button, .inactive-nav-tab {"
        "  background-color: transparent;"
        "  color: %s;"
        "  border: 1px solid transparent;"
        "  border-radius: 8px;"
        "  padding: 5px 10px;"
        "}"
        "stackswitcher button:hover, .inactive-nav-tab:hover {"
        "  background-color: rgba(%s, 0.18);"
        "  color: %s;"
        "}"
        "stackswitcher button:checked, stackswitcher button:active, .active-nav-tab {"
        "  background-color: %s;"
        "  color: #11111b;"
        "  border: 1px solid %s;"
        "  border-radius: 8px;"
        "  font-weight: bold;"
        "}"
        ".play-main-btn {"
        "  background-color: %s;"
        "  color: #11111b;"
        "  font-size: 15px;"
        "  padding: 6px 16px;"
        "  border-radius: 10px;"
        "}"
        ".play-main-btn:hover {"
        "  background-color: %s;"
        "  color: #11111b;"
        "}"
        ".toggle-on {"
        "  background-color: rgba(%s, 0.28);"
        "  color: %s;"
        "  border-color: %s;"
        "}"
        ".liked-btn {"
        "  color: #f38ba8;"
        "  border-color: rgba(243, 139, 168, 0.5);"
        "}"
        "entry {"
        "  background-color: %s;"
        "  color: %s;"
        "  border: 1px solid rgba(%s, 0.35);"
        "  border-radius: 8px;"
        "  padding: 6px 10px;"
        "}"
        "entry:focus {"
        "  border-color: %s;"
        "}"
        "list {"
        "  background-color: %s;"
        "  border-radius: 10px;"
        "}"
        "row {"
        "  background-color: transparent;"
        "  border-bottom: 1px solid rgba(%s, 0.14);"
        "}"
        "row:hover {"
        "  background-color: rgba(%s, 0.12);"
        "}"
        ".track-row {"
        "  padding: 6px 8px;"
        "}"
        ".active-queue-row {"
        "  background-color: rgba(%s, 0.18);"
        "  border-left: 3px solid %s;"
        "  border-radius: 6px;"
        "}"
        ".drag-grip {"
        "  color: %s;"
        "  font-size: 13px;"
        "}"
        ".dragging-row {"
        "  background-color: rgba(%s, 0.28);"
        "  border-radius: 6px;"
        "}"
        ".swiping-remove-row {"
        "  background-color: rgba(243, 139, 168, 0.24);"
        "  border-left: 3px solid #f38ba8;"
        "  border-radius: 6px;"
        "}"
        ".swiping-remove-armed {"
        "  background-color: rgba(243, 139, 168, 0.50);"
        "  border: 1px solid #f38ba8;"
        "  border-left: 4px solid #f38ba8;"
        "  border-radius: 8px;"
        "}"
        ".swiping-remove-armed .drag-grip {"
        "  color: #f38ba8;"
        "  font-weight: bold;"
        "}"
        ".drop-target-above {"
        "  border-top: 2px solid %s;"
        "  background-color: rgba(%s, 0.12);"
        "}"
        ".drop-target-below {"
        "  border-bottom: 2px solid %s;"
        "  background-color: rgba(%s, 0.12);"
        "}"
        ".row-title {"
        "  font-size: 12px;"
        "  font-weight: bold;"
        "  color: %s;"
        "}"
        ".row-btn {"
        "  padding: 3px 8px;"
        "  font-size: 11px;"
        "}"
        ".row-btn-primary {"
        "  background-color: rgba(%s, 0.25);"
        "  color: %s;"
        "  border-color: rgba(%s, 0.50);"
        "  padding: 3px 10px;"
        "}"
        ".row-btn-primary:hover {"
        "  background-color: %s;"
        "  color: #11111b;"
        "}"
        ".section-lbl {"
        "  font-size: 12px;"
        "  font-weight: bold;"
        "  color: %s;"
        "}"
        "scale contents trough {"
        "  background-color: %s;"
        "  border-radius: 6px;"
        "  min-height: 6px;"
        "}"
        "scale contents trough highlight {"
        "  background-color: %s;"
        "  border-radius: 6px;"
        "}"
        "scale contents trough slider {"
        "  background-color: %s;"
        "  border-radius: 50%%;"
        "  min-width: 12px;"
        "  min-height: 12px;"
        "}"
        "textview, textview text {"
        "  background-color: %s;"
        "  color: %s;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "}",
        text_main,
        bg_main, text_main, accent,
        bg_card, accent_rgba,
        text_main,
        artist_color,
        text_sub,
        bg_btn, text_main, accent_rgba,
        accent,
        bg_card, accent_rgba,
        text_sub,
        accent_rgba, text_main,
        accent, accent_hover,
        accent,
        accent_hover,
        accent_rgba, accent, accent,
        bg_card, text_main, accent_rgba,
        accent,
        bg_card,
        accent_rgba,
        accent_rgba,
        accent_rgba, accent,
        text_sub,
        accent_rgba,
        accent, accent_rgba,
        accent, accent_rgba,
        text_main,
        accent_rgba, accent, accent_rgba,
        accent,
        accent,
        bg_btn,
        accent,
        text_main,
        bg_card, text_main
    );

    gtk_css_provider_load_from_data(app.css_provider, css, -1, NULL);
    update_theme_buttons_ui();
    update_switcher_active_style();
}

static void save_theme_to_disk(const ColorTheme *t) {
    char path[512];
    get_theme_file_path(path, sizeof(path));
    char json_buf[2048];
    snprintf(json_buf, sizeof(json_buf),
        "{\n"
        "  \"theme\": \"%s\",\n"
        "  \"label\": \"%s\",\n"
        "  \"bg_main\": \"%s\",\n"
        "  \"bg_card\": \"%s\",\n"
        "  \"bg_btn\": \"%s\",\n"
        "  \"text_main\": \"%s\",\n"
        "  \"text_sub\": \"%s\",\n"
        "  \"accent\": \"%s\",\n"
        "  \"accent_hover\": \"%s\",\n"
        "  \"accent_rgba\": \"%s\",\n"
        "  \"artist_color\": \"%s\"\n"
        "}\n",
        t->id, t->label, t->bg_main, t->bg_card, t->bg_btn,
        t->text_main, t->text_sub, t->accent, t->accent_hover,
        t->accent_rgba, t->artist_color
    );
    g_file_set_contents(path, json_buf, -1, NULL);

    /* Sync Waybar theme colors in background */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s sync-waybar-theme >/dev/null 2>&1 &", CTL_SCRIPT);
    int r = system(cmd);
    (void)r;
}

static void select_theme_by_index(int idx, gboolean save) {
    if (idx < 0 || idx >= NUM_THEMES) idx = 0;
    const ColorTheme *t = &THEMES[idx];
    strncpy(app.current_theme_id, t->id, sizeof(app.current_theme_id) - 1);
    apply_theme_palette(
        t->bg_main, t->bg_card, t->bg_btn,
        t->text_main, t->text_sub,
        t->accent, t->accent_hover, t->accent_rgba, t->artist_color
    );
    if (save) {
        save_theme_to_disk(t);
    }
}

static void on_theme_btn_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int idx = GPOINTER_TO_INT(user_data);
    select_theme_by_index(idx, TRUE);
}

static void apply_css(void) {
    char path[512];
    get_theme_file_path(path, sizeof(path));
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_file(parser, path, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *obj = json_node_get_object(root);
                const char *tid = j_str(obj, "theme", "mocha");
                if (strcmp(tid, "custom") == 0) {
                    strncpy(app.current_theme_id, "custom", sizeof(app.current_theme_id) - 1);
                    apply_theme_palette(
                        j_str(obj, "bg_main", THEMES[0].bg_main),
                        j_str(obj, "bg_card", THEMES[0].bg_card),
                        j_str(obj, "bg_btn", THEMES[0].bg_btn),
                        j_str(obj, "text_main", THEMES[0].text_main),
                        j_str(obj, "text_sub", THEMES[0].text_sub),
                        j_str(obj, "accent", THEMES[0].accent),
                        j_str(obj, "accent_hover", THEMES[0].accent_hover),
                        j_str(obj, "accent_rgba", THEMES[0].accent_rgba),
                        j_str(obj, "artist_color", THEMES[0].artist_color)
                    );
                    g_object_unref(parser);
                    return;
                }
                for (int i = 0; i < NUM_THEMES; i++) {
                    if (strcmp(tid, THEMES[i].id) == 0) {
                        select_theme_by_index(i, FALSE);
                        g_object_unref(parser);
                        return;
                    }
                }
            }
        }
        g_object_unref(parser);
    }
    select_theme_by_index(0, TRUE);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    apply_css();

    strcpy(app.current_filter, "songs");
    app.last_queue_idx = -1;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "YouTube Music");
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);

    GdkScreen *screen = gtk_widget_get_screen(app.window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual && gdk_screen_is_composited(screen)) {
        gtk_widget_set_visual(app.window, visual);
    }
    gtk_widget_set_app_paintable(app.window, TRUE);

    if (gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(app.window));
        gtk_layer_set_layer(GTK_WINDOW(app.window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(app.window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        gtk_layer_set_anchor(GTK_WINDOW(app.window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(app.window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(app.window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(app.window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    }

    gtk_widget_add_events(app.window, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(app.window, "button-press-event", G_CALLBACK(on_window_button_press), NULL);

    GtkWidget *card_guard_evbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(card_guard_evbox), FALSE);
    gtk_widget_set_halign(card_guard_evbox, GTK_ALIGN_START);
    gtk_widget_set_valign(card_guard_evbox, GTK_ALIGN_END);
    gtk_widget_set_margin_start(card_guard_evbox, 265);
    gtk_widget_set_margin_bottom(card_guard_evbox, 10);
    g_signal_connect(card_guard_evbox, "button-press-event", G_CALLBACK(on_card_guard_button_press), NULL);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    app.main_box = main_box;
    gtk_widget_set_size_request(main_box, 510, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(main_box), "main-box");
    gtk_container_add(GTK_CONTAINER(card_guard_evbox), main_box);
    gtk_container_add(GTK_CONTAINER(app.window), card_guard_evbox);

    /* --- Header Bar --- */
    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *brand_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand_lbl), "<span color='#f38ba8'><b>󰎆</b></span> <b>YouTube Music</b>");
    app.auth_badge_lbl = gtk_label_new("");

    app.btn_expand = gtk_button_new_with_label("󰁝 Expandir");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.btn_expand), "row-btn-primary");
    g_signal_connect(app.btn_expand, "clicked", G_CALLBACK(on_expand_btn_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(top_bar), brand_lbl, FALSE, FALSE, 0);
    gtk_box_set_center_widget(GTK_BOX(top_bar), app.auth_badge_lbl);
    gtk_box_pack_end(GTK_BOX(top_bar), app.btn_expand, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), top_bar, FALSE, FALSE, 0);

    /* --- Now Playing Card --- */
    GtkWidget *player_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(player_card), "player-card");

    GtkWidget *np_event_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(np_event_box), FALSE);
    gtk_widget_set_tooltip_text(np_event_box, "Clique aqui para expandir / recolher o player completo");
    g_signal_connect(np_event_box, "button-press-event", G_CALLBACK(on_np_card_button_press), NULL);

    GtkWidget *np_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    app.cover_img = gtk_image_new();
    gtk_widget_set_size_request(app.cover_img, 56, 56);
    gtk_box_pack_start(GTK_BOX(np_top), app.cover_img, FALSE, FALSE, 0);

    GtkWidget *np_meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(np_meta, GTK_ALIGN_CENTER);

    app.title_lbl = gtk_label_new("Nenhuma música tocando");
    gtk_label_set_xalign(GTK_LABEL(app.title_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app.title_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app.title_lbl), 35);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.title_lbl), "song-title");

    app.artist_lbl = gtk_label_new("Clique aqui ou em Expandir para abrir o player");
    gtk_label_set_xalign(GTK_LABEL(app.artist_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app.artist_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app.artist_lbl), 38);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.artist_lbl), "song-artist");

    app.album_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app.album_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app.album_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app.album_lbl), 38);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.album_lbl), "song-album");

    gtk_box_pack_start(GTK_BOX(np_meta), app.title_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(np_meta), app.artist_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(np_meta), app.album_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(np_top), np_meta, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(np_event_box), np_top);
    gtk_box_pack_start(GTK_BOX(player_card), np_event_box, FALSE, FALSE, 0);

    /* Progress Bar Row */
    GtkWidget *prog_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.pos_lbl = gtk_label_new("00:00");
    app.dur_lbl = gtk_label_new("00:00");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.pos_lbl), "time-lbl");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.dur_lbl), "time-lbl");

    app.pos_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(app.pos_scale), FALSE);
    gtk_widget_add_events(app.pos_scale, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(app.pos_scale, "button-press-event", G_CALLBACK(on_pos_button_press), NULL);
    g_signal_connect(app.pos_scale, "motion-notify-event", G_CALLBACK(on_pos_motion_notify), NULL);
    g_signal_connect(app.pos_scale, "button-release-event", G_CALLBACK(on_pos_button_release), NULL);

    gtk_box_pack_start(GTK_BOX(prog_box), app.pos_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(prog_box), app.pos_scale, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(prog_box), app.dur_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(player_card), prog_box, FALSE, FALSE, 0);

    /* Controls Row */
    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.btn_shuffle = gtk_button_new_with_label("󰒝");
    gtk_widget_set_tooltip_text(app.btn_shuffle, "Modo Aleatório (Shuffle)");
    app.btn_prev = gtk_button_new_with_label("󰒮");
    app.btn_play = gtk_button_new_with_label("󰐊");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.btn_play), "play-main-btn");
    app.btn_next = gtk_button_new_with_label("󰒭");
    app.btn_repeat = gtk_button_new_with_label("󰑖");
    gtk_widget_set_tooltip_text(app.btn_repeat, "Repetição (Desligada / Música / Fila)");
    app.btn_like = gtk_button_new_with_label("♡");
    gtk_widget_set_tooltip_text(app.btn_like, "Curtir música");
    app.btn_radio = gtk_button_new_with_label("󰐹 Rádio");
    gtk_widget_set_tooltip_text(app.btn_radio, "Rádio Automática (adiciona músicas similares)");

    g_signal_connect(app.btn_shuffle, "clicked", G_CALLBACK(on_shuffle_clicked), NULL);
    g_signal_connect(app.btn_prev, "clicked", G_CALLBACK(on_prev_clicked), NULL);
    g_signal_connect(app.btn_play, "clicked", G_CALLBACK(on_play_clicked), NULL);
    g_signal_connect(app.btn_next, "clicked", G_CALLBACK(on_next_clicked), NULL);
    g_signal_connect(app.btn_repeat, "clicked", G_CALLBACK(on_repeat_clicked), NULL);
    g_signal_connect(app.btn_like, "clicked", G_CALLBACK(on_like_current_clicked), NULL);
    g_signal_connect(app.btn_radio, "clicked", G_CALLBACK(on_radio_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_shuffle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_repeat, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_like, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), app.btn_radio, FALSE, FALSE, 0);

    GtkWidget *vol_icon = gtk_label_new("󰕾");
    app.vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 2.0);
    gtk_scale_set_draw_value(GTK_SCALE(app.vol_scale), FALSE);
    gtk_widget_set_size_request(app.vol_scale, 85, -1);
    g_signal_connect(app.vol_scale, "value-changed", G_CALLBACK(on_vol_value_changed), NULL);

    gtk_box_pack_end(GTK_BOX(ctrl_box), app.vol_scale, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(ctrl_box), vol_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(player_card), ctrl_box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_box), player_card, FALSE, FALSE, 0);

    /* --- Navigation Tabs (Wrapped in expanded_box) --- */
    app.expanded_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    app.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app.stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(app.stack), 160);

    GtkWidget *switcher = gtk_stack_switcher_new();
    app.switcher = switcher;
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(app.stack));
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(app.expanded_box), switcher, FALSE, FALSE, 0);

    /* === TAB 0: HOME (INÍCIO) === */
    GtkWidget *home_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *home_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.home_status_lbl = gtk_label_new("Para você • Recomendações e mais ouvidas");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.home_status_lbl), "row-sub");
    GtkWidget *btn_ref_home = gtk_button_new_with_label("󰑐 Atualizar");
    g_signal_connect(btn_ref_home, "clicked", G_CALLBACK(on_refresh_home_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(home_hdr), app.home_status_lbl, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(home_hdr), btn_ref_home, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(home_tab), home_hdr, FALSE, FALSE, 0);

    GtkWidget *home_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(home_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    app.home_sections_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(home_scroll), app.home_sections_box);
    gtk_box_pack_start(GTK_BOX(home_tab), home_scroll, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(app.stack), home_tab, "home", "󰋜 Início");

    /* === TAB 1: SEARCH === */
    GtkWidget *search_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *search_bar_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.search_entry), "Digite para buscar em tempo real...");
    g_signal_connect(app.search_entry, "activate", G_CALLBACK(trigger_search), NULL);
    g_signal_connect(app.search_entry, "changed", G_CALLBACK(on_search_entry_changed), NULL);

    GtkWidget *btn_do_search = gtk_button_new_with_label("󰍉 Buscar");
    g_signal_connect(btn_do_search, "clicked", G_CALLBACK(trigger_search), NULL);

    gtk_box_pack_start(GTK_BOX(search_bar_row), app.search_entry, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(search_bar_row), btn_do_search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_tab), search_bar_row, FALSE, FALSE, 0);

    /* Live search suggestions bar */
    app.search_suggestions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(search_tab), app.search_suggestions_box, FALSE, FALSE, 0);

    /* Filter row */
    GtkWidget *filter_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.filter_songs_btn = gtk_button_new_with_label("🎵 Músicas");
    app.filter_albums_btn = gtk_button_new_with_label("💿 Álbuns");
    app.filter_playlists_btn = gtk_button_new_with_label("📑 Playlists");

    g_signal_connect(app.filter_songs_btn, "clicked", G_CALLBACK(on_filter_btn_clicked), "songs");
    g_signal_connect(app.filter_albums_btn, "clicked", G_CALLBACK(on_filter_btn_clicked), "albums");
    g_signal_connect(app.filter_playlists_btn, "clicked", G_CALLBACK(on_filter_btn_clicked), "playlists");
    update_filter_buttons_style();

    app.search_status_lbl = gtk_label_new("Digite acima e pressione Enter para buscar");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.search_status_lbl), "row-sub");

    app.btn_clear_search_hist = gtk_button_new_with_label("󰆴 Limpar");
    gtk_widget_set_tooltip_text(app.btn_clear_search_hist, "Limpar histórico de buscas");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.btn_clear_search_hist), "row-btn");
    g_signal_connect(app.btn_clear_search_hist, "clicked", G_CALLBACK(on_clear_search_hist_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(filter_row), app.filter_songs_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filter_row), app.filter_albums_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filter_row), app.filter_playlists_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(filter_row), app.btn_clear_search_hist, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(filter_row), app.search_status_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_tab), filter_row, FALSE, FALSE, 0);

    /* Collection action bar (hidden unless viewing album/playlist) */
    app.search_back_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.search_collection_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.search_collection_lbl), "section-lbl");
    GtkWidget *btn_play_col = gtk_button_new_with_label("󰐊 Tocar Tudo");
    GtkWidget *btn_shuf_col = gtk_button_new_with_label("󰒝 Aleatório");
    g_signal_connect(btn_play_col, "clicked", G_CALLBACK(on_play_collection_all_clicked), GINT_TO_POINTER(0));
    g_signal_connect(btn_shuf_col, "clicked", G_CALLBACK(on_play_collection_all_clicked), GINT_TO_POINTER(1));
    gtk_box_pack_start(GTK_BOX(app.search_back_box), app.search_collection_lbl, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app.search_back_box), btn_shuf_col, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app.search_back_box), btn_play_col, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_tab), app.search_back_box, FALSE, FALSE, 0);

    GtkWidget *search_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    app.search_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.search_list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(search_scroll), app.search_list);
    gtk_box_pack_start(GTK_BOX(search_tab), search_scroll, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(app.stack), search_tab, "search", "󰍉 Buscar");

    /* === TAB 2: QUEUE === */
    GtkWidget *queue_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *queue_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.queue_count_lbl = gtk_label_new("Fila de Reprodução");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.queue_count_lbl), "section-lbl");
    GtkWidget *btn_clear_q = gtk_button_new_with_label("󰆴 Limpar Fila");
    g_signal_connect(btn_clear_q, "clicked", G_CALLBACK(on_clear_queue_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(queue_hdr), app.queue_count_lbl, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(queue_hdr), btn_clear_q, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(queue_tab), queue_hdr, FALSE, FALSE, 0);

    GtkWidget *queue_scroll = gtk_scrolled_window_new(NULL, NULL);
    app.queue_scroll = queue_scroll;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(queue_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    app.queue_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.queue_list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(queue_scroll), app.queue_list);
    gtk_box_pack_start(GTK_BOX(queue_tab), queue_scroll, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(app.stack), queue_tab, "queue", "󰲸 Fila");

    /* === TAB 3: LIKED SONGS === */
    GtkWidget *liked_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *liked_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.liked_status_lbl = gtk_label_new("Músicas Curtidas");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.liked_status_lbl), "section-lbl");

    GtkWidget *btn_play_liked = gtk_button_new_with_label("󰐊 Tocar Todas");
    GtkWidget *btn_shuf_liked = gtk_button_new_with_label("󰒝 Aleatório");
    GtkWidget *btn_ref_liked = gtk_button_new_with_label("󰑐");
    g_signal_connect(btn_play_liked, "clicked", G_CALLBACK(on_play_all_liked_clicked), GINT_TO_POINTER(0));
    g_signal_connect(btn_shuf_liked, "clicked", G_CALLBACK(on_play_all_liked_clicked), GINT_TO_POINTER(1));
    g_signal_connect(btn_ref_liked, "clicked", G_CALLBACK(refresh_liked_tab), NULL);

    gtk_box_pack_start(GTK_BOX(liked_hdr), app.liked_status_lbl, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(liked_hdr), btn_ref_liked, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(liked_hdr), btn_shuf_liked, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(liked_hdr), btn_play_liked, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(liked_tab), liked_hdr, FALSE, FALSE, 0);

    GtkWidget *liked_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(liked_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    app.liked_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.liked_list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(liked_scroll), app.liked_list);
    gtk_box_pack_start(GTK_BOX(liked_tab), liked_scroll, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(app.stack), liked_tab, "liked", "♥ Curtidas");

    /* === TAB 4: LIBRARY & HISTORY === */
    GtkWidget *lib_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *lib_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app.lib_status_lbl = gtk_label_new("Suas Playlists & Histórico");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.lib_status_lbl), "row-sub");
    GtkWidget *btn_ref_lib = gtk_button_new_with_label("󰑐 Atualizar");
    g_signal_connect(btn_ref_lib, "clicked", G_CALLBACK(refresh_library_tab), NULL);
    gtk_box_pack_start(GTK_BOX(lib_hdr), app.lib_status_lbl, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(lib_hdr), btn_ref_lib, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lib_tab), lib_hdr, FALSE, FALSE, 0);

    GtkWidget *lib_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lib_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    GtkWidget *lib_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *pl_title = gtk_label_new("📑 Suas Playlists do YouTube Music");
    gtk_label_set_xalign(GTK_LABEL(pl_title), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(pl_title), "section-lbl");
    app.lib_playlists_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.lib_playlists_list), GTK_SELECTION_NONE);

    GtkWidget *hist_title = gtk_label_new("🕒 Tocadas Recentemente");
    gtk_label_set_xalign(GTK_LABEL(hist_title), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(hist_title), "section-lbl");
    app.lib_history_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.lib_history_list), GTK_SELECTION_NONE);

    gtk_box_pack_start(GTK_BOX(lib_inner), pl_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lib_inner), app.lib_playlists_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lib_inner), hist_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lib_inner), app.lib_history_list, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(lib_scroll), lib_inner);
    gtk_box_pack_start(GTK_BOX(lib_tab), lib_scroll, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(app.stack), lib_tab, "library", "󰉋 Biblioteca");

    /* === TAB 5: CONFIG (THEMES & ACCOUNT) === */
    GtkWidget *acc_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *theme_title = gtk_label_new("🎨 Tema de Cores (salvo em ~/.config/waybar-ytmusic/theme.json)");
    gtk_label_set_xalign(GTK_LABEL(theme_title), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(theme_title), "section-lbl");
    gtk_box_pack_start(GTK_BOX(acc_tab), theme_title, FALSE, FALSE, 0);

    GtkWidget *theme_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(theme_grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(theme_grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(theme_grid), TRUE);

    for (int i = 0; i < NUM_THEMES; i++) {
        GtkWidget *tbtn = gtk_button_new_with_label(THEMES[i].label);
        app.theme_btns[i] = tbtn;
        g_signal_connect(tbtn, "clicked", G_CALLBACK(on_theme_btn_clicked), GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(theme_grid), tbtn, i % 2, i / 2, 1, 1);
    }
    update_theme_buttons_ui();
    gtk_box_pack_start(GTK_BOX(acc_tab), theme_grid, FALSE, FALSE, 2);

    GtkWidget *acc_title = gtk_label_new("󰀄 Conta do YouTube Music");
    gtk_label_set_xalign(GTK_LABEL(acc_title), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(acc_title), "section-lbl");
    gtk_box_pack_start(GTK_BOX(acc_tab), acc_title, FALSE, FALSE, 2);

    GtkWidget *acc_info = gtk_label_new(
        "Conecte sua conta para sincronizar playlists e músicas curtidas na nuvem:"
    );
    gtk_label_set_xalign(GTK_LABEL(acc_info), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(acc_info), "row-sub");
    gtk_box_pack_start(GTK_BOX(acc_tab), acc_info, FALSE, FALSE, 0);

    GtkWidget *import_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *btn_imp_auto = gtk_button_new_with_label("󰑐 Auto-Detectar");
    GtkWidget *btn_imp_ff = gtk_button_new_with_label(" Firefox / Zen");
    GtkWidget *btn_imp_ch = gtk_button_new_with_label(" Chrome / Brave");
    GtkWidget *btn_logout = gtk_button_new_with_label("󰍃 Sair");

    g_signal_connect(btn_imp_auto, "clicked", G_CALLBACK(on_import_browser_clicked), "auto");
    g_signal_connect(btn_imp_ff, "clicked", G_CALLBACK(on_import_browser_clicked), "firefox");
    g_signal_connect(btn_imp_ch, "clicked", G_CALLBACK(on_import_browser_clicked), "chrome");
    g_signal_connect(btn_logout, "clicked", G_CALLBACK(on_logout_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(import_row), btn_imp_auto, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(import_row), btn_imp_ff, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(import_row), btn_imp_ch, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(import_row), btn_logout, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(acc_tab), import_row, FALSE, FALSE, 0);

    app.auth_msg_lbl = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(app.auth_msg_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(app.auth_msg_lbl), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.auth_msg_lbl), "section-lbl");
    gtk_box_pack_start(GTK_BOX(acc_tab), app.auth_msg_lbl, FALSE, FALSE, 0);

    GtkWidget *hdr_lbl = gtk_label_new("Ou cole os Request Headers / Cookie de music.youtube.com:");
    gtk_label_set_xalign(GTK_LABEL(hdr_lbl), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(hdr_lbl), "row-sub");
    gtk_box_pack_start(GTK_BOX(acc_tab), hdr_lbl, FALSE, FALSE, 0);

    GtkWidget *tv_scroll = gtk_scrolled_window_new(NULL, NULL);
    app.headers_textview = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app.headers_textview), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(tv_scroll), app.headers_textview);
    gtk_box_pack_start(GTK_BOX(acc_tab), tv_scroll, TRUE, TRUE, 0);

    GtkWidget *btn_save_hdr = gtk_button_new_with_label("󰄬 Salvar Headers Manualmente");
    g_signal_connect(btn_save_hdr, "clicked", G_CALLBACK(on_save_headers_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(acc_tab), btn_save_hdr, FALSE, FALSE, 0);

    gtk_stack_add_titled(GTK_STACK(app.stack), acc_tab, "account", "󰏘 Config");

    g_signal_connect(app.stack, "notify::visible-child-name", G_CALLBACK(on_stack_visible_child_changed), NULL);
    gtk_box_pack_start(GTK_BOX(app.expanded_box), app.stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), app.expanded_box, TRUE, TRUE, 0);

    gtk_widget_show_all(app.window);
    gtk_widget_hide(app.search_back_box);
    gtk_widget_hide(app.search_suggestions_box);
    gtk_widget_hide(app.btn_clear_search_hist);

    /* Start in Compact Mini-Player mode; click card or Expandir to open full tabs */
    poll_and_update();
    set_expanded_mode(FALSE);
    update_switcher_active_style();
    app.poll_timer_id = g_timeout_add(1000, on_poll_timer, NULL);

    gtk_main();
    return 0;
}
