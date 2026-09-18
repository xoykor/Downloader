/*
 * Interface GTK4.
 *
 * GTK só é manipulado na thread principal. Workers recebem cópias próprias das
 * tarefas, executam a engine em `GThreadPool` e devolvem dados por `g_idle_add`,
 * que agenda a atualização visual de volta na thread do loop principal.
 */

#include "downloader/engine.h"

#include <gtk/gtk.h>
#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct DesktopApp DesktopApp;

/*
 * Trabalho entregue a um pool. `task` é cópia profunda; `cancelled` vive tanto
 * quanto o job e também é referenciado pela tabela de cancelamento da UI.
 */
typedef struct {
    DesktopApp *app;
    DldTaskRecord task;
    atomic_bool *cancelled;
} TaskJob;

/*
 * Mensagem atravessando worker -> thread GTK. Todas as strings são cópias GLib
 * porque o evento original da engine só é válido durante o callback.
 */
typedef struct {
    DesktopApp *app;
    char *task_id;
    DldTaskStatus status;
    gboolean has_progress;
    double progress;
    char *speed;
    char *message;
    char *path;
} UiUpdate;

/*
 * Widgets associados a uma tarefa ou faixa. A hash table possui apenas esta
 * pequena struct; os widgets continuam pertencendo à árvore GTK.
 */
typedef struct {
    GtkLabel *label;
    GtkProgressBar *progress;
    GtkWidget *cancel_button;
} QueueRowUi;

/*
 * Pedido de análise também possui suas próprias cópias, pois a caixa de texto
 * da interface pode mudar enquanto o worker ainda está consultando o yt-dlp.
 */
typedef struct {
    DesktopApp *app;
    char *url;
    gboolean playlist;
    bool has_auth;
    DldAuthRef auth;
} AnalyzeJob;

struct DesktopApp {
    GtkApplication *application;
    GtkWindow *window;
    GtkStack *stack;
    GtkLabel *status_label;
    GtkLabel *sidebar_output_label;
    GtkListBox *queue_list;

    GtkEntry *download_url;
    GtkComboBoxText *download_type;
    GtkComboBoxText *download_format;
    GtkComboBoxText *download_quality;
    GtkComboBoxText *download_bitrate;
    GtkCheckButton *download_playlist;
    GtkComboBoxText *auth_kind;
    GtkEntry *auth_value;
    GtkEntry *download_destination;

    GtkEntry *convert_input;
    GtkComboBoxText *convert_format;
    GtkComboBoxText *convert_acceleration;
    GtkEntry *convert_destination;

    GtkEntry *settings_output;
    GtkSwitch *settings_youtube_protection;

    DldEngine engine;
    GThreadPool *download_pool;
    GThreadPool *conversion_pool;
    GThreadPool *analysis_pool;
    atomic_bool shutting_down;
    GHashTable *cancel_flags; /* task id -> atomic_bool*; ownership do TaskJob. */
    GHashTable *queue_rows;   /* task/faixa id -> QueueRowUi*. */
    GMutex jobs_mutex;
};

/*
 * Paleta da interface Rust original, reproduzida em CSS GTK4.
 *
 * Mantemos o tema dentro do binário para o AppImage ter a mesma aparência em
 * qualquer distribuição, sem depender do tema GTK instalado pelo usuário.
 */
static const char *APP_CSS =
    "window, .app-root { background-color: #0c121a; color: #e8eff4; }\n"
    ".sidebar-shell { background-color: #101720; padding: 18px; }\n"
    ".brand { color: #68dec3; font-size: 17px; font-weight: bold; }\n"
    ".brand-subtitle, .muted { color: #97a9b8; }\n"
    ".sidebar-caption { color: #6f8394; font-size: 11px; font-weight: bold; }\n"
    "stacksidebar { background-color: transparent; }\n"
    "stacksidebar list { background-color: transparent; }\n"
    "stacksidebar row { min-height: 44px; border-radius: 8px; margin-bottom: 5px; }\n"
    "stacksidebar row:selected { background-color: #1c373a; }\n"
    "stacksidebar row label { color: #e8eff4; font-size: 15px; }\n"
    "stacksidebar row:selected label { color: #68dec3; font-weight: bold; }\n"
    ".page { background-color: #0c121a; padding: 28px; }\n"
    ".page-title { color: #e8eff4; font-size: 28px; font-weight: bold; }\n"
    ".page-subtitle { color: #97a9b8; font-size: 14px; }\n"
    ".card { background-color: #17202b; border: 1px solid #232f3d; border-radius: 12px; padding: 16px; }\n"
    ".section-number { color: #68dec3; font-size: 12px; font-weight: bold; }\n"
    ".section-title { color: #e8eff4; font-size: 16px; font-weight: bold; }\n"
    ".field-label { color: #97a9b8; font-size: 12px; }\n"
    ".hint { color: #68dec3; font-size: 12px; }\n"
    "entry, combobox button { background-color: #212e3c; color: #e8eff4; border: 1px solid #31404f; border-radius: 7px; min-height: 38px; }\n"
    "entry:focus, combobox button:focus { border-color: #68dec3; }\n"
    "button { border-radius: 7px; padding: 8px 14px; }\n"
    "button.primary { background-color: #68dec3; color: #0c121a; font-weight: bold; min-height: 40px; min-width: 160px; }\n"
    "button.primary:hover { background-color: #7fe6ce; }\n"
    "button.secondary { background-color: #212e3c; color: #e8eff4; border: 1px solid #31404f; }\n"
    "checkbutton, switch { color: #e8eff4; }\n"
    ".status-bar { background-color: #0c121a; border-top: 1px solid #17202b; padding: 10px 24px; }\n"
    ".status-dot { color: #68dec3; font-size: 16px; }\n"
    ".queue-list, .queue-list row { background-color: transparent; }\n"
    ".queue-row { background-color: #17202b; border: 1px solid #232f3d; border-radius: 12px; padding: 14px; margin: 5px 0; }\n"
    ".queue-title { color: #e8eff4; font-size: 14px; font-weight: bold; }\n"
    ".track-progress { min-height: 8px; }\n"
    ".track-progress trough { background-color: #212e3c; border-radius: 6px; min-height: 8px; }\n"
    ".track-progress progress { background-color: #68dec3; border-radius: 6px; min-height: 8px; }\n"
    ".track-progress text { color: #97a9b8; font-size: 11px; }\n";

static void apply_theme(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, APP_CSS, -1);

    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

static char *make_task_id(const char *prefix)
{
    struct timespec now;
    (void)timespec_get(&now, TIME_UTC);
    char buffer[96];
    (void)snprintf(buffer, sizeof(buffer), "%s-%lld-%ld", prefix,
                   (long long)now.tv_sec, now.tv_nsec);
    return dld_string_duplicate(buffer);
}

static void set_status(DesktopApp *app, const char *text)
{
    gtk_label_set_text(app->status_label, text != NULL ? text : "");
}

static const char *status_label(DldTaskStatus status)
{
    switch (status) {
    case DLD_STATUS_QUEUED: return "Na fila";
    case DLD_STATUS_ANALYZING: return "Analisando";
    case DLD_STATUS_WAITING_AUTH: return "Aguardando autenticação";
    case DLD_STATUS_DOWNLOADING: return "Baixando";
    case DLD_STATUS_MERGING: return "Unindo";
    case DLD_STATUS_CONVERTING: return "Convertendo";
    case DLD_STATUS_VALIDATING: return "Validando";
    case DLD_STATUS_COMPLETED: return "Concluído";
    case DLD_STATUS_FAILED: return "Falhou";
    case DLD_STATUS_CANCELLED: return "Cancelado";
    case DLD_STATUS_INTERRUPTED: return "Interrompido";
    default: return "Desconhecido";
    }
}

static void cancel_clicked(GtkButton *button, gpointer userdata);

static gboolean status_is_terminal(DldTaskStatus status)
{
    return status == DLD_STATUS_COMPLETED ||
           status == DLD_STATUS_FAILED ||
           status == DLD_STATUS_CANCELLED;
}

static char *parent_task_id_copy(const char *task_id)
{
    if (task_id == NULL) return NULL;

    const char *separator = strstr(task_id, "::");
    const size_t length =
        separator != NULL ? (size_t)(separator - task_id) : strlen(task_id);

    char *parent = g_malloc(length + 1U);
    memcpy(parent, task_id, length);
    parent[length] = '\0';
    return parent;
}

static QueueRowUi *ensure_queue_row(DesktopApp *app,
                                    const char *task_id,
                                    DldTaskStatus status)
{
    QueueRowUi *row = g_hash_table_lookup(app->queue_rows, task_id);
    if (row != NULL) return row;

    row = g_new0(QueueRowUi, 1);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_add_css_class(card, "queue-row");

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    row->label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(row->label, 0.0f);
    gtk_label_set_wrap(row->label, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(row->label), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(row->label), "queue-title");

    row->cancel_button = gtk_button_new_with_label("Cancelar");
    gtk_widget_add_css_class(row->cancel_button, "secondary");

    /*
     * Uma faixa usa "tarefa::id-da-faixa" para ter progresso próprio, mas o
     * cancelamento sempre aponta para a tarefa pai que possui o processo yt-dlp.
     */
    char *parent_id = parent_task_id_copy(task_id);
    g_object_set_data_full(
        G_OBJECT(row->cancel_button),
        "task-id",
        parent_id,
        g_free);
    g_signal_connect(
        row->cancel_button,
        "clicked",
        G_CALLBACK(cancel_clicked),
        app);

    gtk_box_append(GTK_BOX(header), GTK_WIDGET(row->label));
    gtk_box_append(GTK_BOX(header), row->cancel_button);
    gtk_box_append(GTK_BOX(card), header);

    row->progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(row->progress, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(row->progress), "track-progress");
    gtk_widget_set_visible(
        GTK_WIDGET(row->progress),
        strstr(task_id, "::") != NULL);
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(row->progress));

    gtk_list_box_append(app->queue_list, card);
    g_hash_table_insert(app->queue_rows, g_strdup(task_id), row);

    char initial[640];
    (void)snprintf(
        initial,
        sizeof(initial),
        "%s  —  %s",
        task_id,
        status_label(status));
    gtk_label_set_text(row->label, initial);
    return row;
}

static void update_queue_row(QueueRowUi *row,
                             const UiUpdate *update)
{
    const bool child = strstr(update->task_id, "::") != NULL;
    const char *name =
        child && update->message != NULL && *update->message != '\0'
            ? update->message
            : update->task_id;

    GString *title = g_string_new(name);
    g_string_append_printf(
        title,
        "  —  %s",
        status_label(update->status));

    if (!child &&
        update->message != NULL &&
        *update->message != '\0' &&
        strcmp(update->message, name) != 0) {
        g_string_append_printf(title, "  ·  %s", update->message);
    }

    if (update->path != NULL && *update->path != '\0') {
        g_string_append_printf(title, "\n%s", update->path);
    }

    gtk_label_set_text(row->label, title->str);
    g_string_free(title, TRUE);

    if (update->has_progress) {
        double fraction = update->progress / 100.0;
        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;

        gtk_progress_bar_set_fraction(row->progress, fraction);
        gtk_widget_set_visible(GTK_WIDGET(row->progress), TRUE);

        char progress_text[160];
        if (update->speed != NULL && *update->speed != '\0') {
            (void)snprintf(
                progress_text,
                sizeof(progress_text),
                "%.0f%%  ·  %s",
                update->progress,
                update->speed);
        } else {
            (void)snprintf(
                progress_text,
                sizeof(progress_text),
                "%.0f%%",
                update->progress);
        }
        gtk_progress_bar_set_text(row->progress, progress_text);
    } else if (child && update->status == DLD_STATUS_VALIDATING) {
        gtk_widget_set_visible(GTK_WIDGET(row->progress), TRUE);
        gtk_progress_bar_set_fraction(row->progress, 1.0);
        gtk_progress_bar_set_text(row->progress, "Validando…");
    }

    const gboolean terminal = status_is_terminal(update->status);
    gtk_widget_set_visible(row->cancel_button, !terminal);

    if (update->status == DLD_STATUS_COMPLETED) {
        gtk_widget_set_visible(GTK_WIDGET(row->progress), TRUE);
        gtk_progress_bar_set_fraction(row->progress, 1.0);
        gtk_progress_bar_set_text(row->progress, "100%");
    }
}

/* Executado pelo main loop GTK, nunca pelo worker que produziu o evento. */
static gboolean apply_ui_update(gpointer data)
{
    UiUpdate *update = data;
    QueueRowUi *row = ensure_queue_row(
        update->app,
        update->task_id,
        update->status);
    update_queue_row(row, update);

    /*
     * Progresso de faixa não deve fazer a barra de status inferior trocar de
     * texto dezenas de vezes por segundo. Ela continua mostrando mensagens da
     * tarefa pai ou estados finais relevantes.
     */
    const bool child = strstr(update->task_id, "::") != NULL;
    if (update->message != NULL &&
        (!child || status_is_terminal(update->status))) {
        set_status(update->app, update->message);
    }

    g_free(update->task_id);
    g_free(update->speed);
    g_free(update->message);
    g_free(update->path);
    g_free(update);
    return G_SOURCE_REMOVE;
}

/* Copia o evento emprestado antes de entregá-lo de forma assíncrona ao GTK. */
static void engine_event(const DldEngineEvent *event, void *userdata)
{
    DesktopApp *app = userdata;
    if (atomic_load(&app->shutting_down)) return;

    UiUpdate *update = g_new0(UiUpdate, 1);
    update->app = app;
    update->task_id =
        g_strdup(event->task_id != NULL ? event->task_id : "tarefa");
    update->status = event->status;
    update->has_progress = event->has_progress;
    update->progress = event->progress_percent;
    update->speed = g_strdup(event->speed);
    update->message = g_strdup(event->message);
    update->path = g_strdup(event->path);
    g_idle_add(apply_ui_update, update);
}

static void task_job_free(TaskJob *job)
{
    if (job == NULL) return;
    dld_task_record_clear(&job->task);
    free(job->cancelled);
    g_free(job);
}

/* Worker não toca em widgets; ele só executa a engine e emite eventos copiados. */
static void task_worker(gpointer data, gpointer user_data)
{
    TaskJob *job = data;
    DesktopApp *app = user_data;
    DldAppError error;
    dld_app_error_init(&error);

    if (atomic_load(job->cancelled)) {
        job->task.status = DLD_STATUS_CANCELLED;
        engine_event(&(DldEngineEvent){
            .task_id = job->task.id,
            .status = DLD_STATUS_CANCELLED,
            .message = "Cancelado antes de iniciar",
        }, app);
    } else {
        (void)dld_engine_execute_task(
            &app->engine,
            &job->task,
            job->cancelled,
            engine_event,
            app,
            &error);
    }

    g_mutex_lock(&app->jobs_mutex);
    g_hash_table_remove(app->cancel_flags, job->task.id);
    g_mutex_unlock(&app->jobs_mutex);

    dld_app_error_clear(&error);
    task_job_free(job);
}

static void cancel_clicked(GtkButton *button, gpointer userdata)
{
    DesktopApp *app = userdata;
    const char *task_id =
        g_object_get_data(G_OBJECT(button), "task-id");
    if (task_id == NULL) return;

    g_mutex_lock(&app->jobs_mutex);
    atomic_bool *flag =
        g_hash_table_lookup(app->cancel_flags, task_id);
    if (flag != NULL) atomic_store(flag, true);
    g_mutex_unlock(&app->jobs_mutex);

    set_status(
        app,
        flag != NULL
            ? "Cancelamento solicitado."
            : "A tarefa não está mais ativa.");
}

static void add_queue_row(DesktopApp *app,
                          const char *task_id,
                          DldTaskStatus status)
{
    (void)ensure_queue_row(app, task_id, status);
}

static bool queue_task(DesktopApp *app, DldTaskRecord *task)
{
    TaskJob *job = g_new0(TaskJob, 1);
    job->app = app;
    dld_task_record_init(&job->task);
    if (!dld_task_record_copy(&job->task, task)) {
        task_job_free(job);
        return false;
    }
    job->cancelled = malloc(sizeof(*job->cancelled));
    if (job->cancelled == NULL) {
        task_job_free(job);
        return false;
    }
    atomic_init(job->cancelled, false);
    job->task.status = DLD_STATUS_QUEUED;
    DldAppError error;
    dld_app_error_init(&error);
    (void)dld_database_put_task(&app->engine.database, &job->task, &error);
    dld_app_error_clear(&error);

    /*
     * A hash table apenas referencia a flag; `TaskJob` continua sendo o dono e a
     * remove da tabela antes de liberar a memória no fim do worker.
     */
    g_mutex_lock(&app->jobs_mutex);
    g_hash_table_replace(app->cancel_flags, g_strdup(job->task.id), job->cancelled);
    g_mutex_unlock(&app->jobs_mutex);
    add_queue_row(app, job->task.id, DLD_STATUS_QUEUED);

    GThreadPool *pool = job->task.kind == DLD_TASK_DOWNLOAD ? app->download_pool : app->conversion_pool;
    g_thread_pool_push(pool, job, NULL);
    return true;
}

static bool auth_from_widgets(DesktopApp *app, DldAuthRef *auth, bool *has_auth)
{
    dld_auth_ref_init(auth);
    *has_auth = false;
    const int active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->auth_kind));
    const char *value = gtk_editable_get_text(GTK_EDITABLE(app->auth_value));
    if (active <= 0 || value == NULL || *value == '\0') return true;
    const DldAuthKind kind = active == 1 ? DLD_AUTH_COOKIE_FILE : DLD_AUTH_BROWSER_PROFILE;
    *has_auth = dld_auth_ref_set(auth, kind, value);
    return *has_auth;
}

static void download_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    DesktopApp *app = userdata;
    const char *url = gtk_editable_get_text(GTK_EDITABLE(app->download_url));
    if (!dld_validate_url(url)) {
        set_status(app, "Informe uma URL http:// ou https:// válida.");
        return;
    }

    const char *type = gtk_combo_box_get_active_id(
        GTK_COMBO_BOX(app->download_type));
    char *format = gtk_combo_box_text_get_active_text(app->download_format);
    char *quality = gtk_combo_box_text_get_active_text(app->download_quality);
    char *bitrate = gtk_combo_box_text_get_active_text(app->download_bitrate);
    const int max_height = quality != NULL && strcmp(quality, "Sem limite") != 0 ? atoi(quality) : 0;

    struct json_object *options = json_object_new_object();
    json_object_object_add(options, "playlist", json_object_new_boolean(
        gtk_check_button_get_active(app->download_playlist)));
    json_object_object_add(options, "media_kind", json_object_new_string(type != NULL ? type : "video+audio"));
    json_object_object_add(options, "output_format", json_object_new_string(format != NULL ? format : "auto"));
    json_object_object_add(options, "bitrate", json_object_new_string(bitrate != NULL ? bitrate : "auto"));
    json_object_object_add(options, "max_height", json_object_new_int(max_height));

    DldTaskRecord task;
    dld_task_record_init(&task);
    task.id = make_task_id("download");
    task.kind = DLD_TASK_DOWNLOAD;
    task.input_url = dld_string_duplicate(url);
    task.options_json = dld_string_duplicate(json_object_to_json_string_ext(options, JSON_C_TO_STRING_PLAIN));
    const char *destination = gtk_editable_get_text(GTK_EDITABLE(app->download_destination));
    if (destination != NULL && *destination != '\0') task.destination = dld_string_duplicate(destination);
    task.collision = DLD_COLLISION_RENAME;
    bool has_auth = false;
    DldAuthRef auth;
    if (auth_from_widgets(app, &auth, &has_auth) && has_auth) {
        task.has_auth = dld_auth_ref_copy(&task.auth, &auth);
    }
    dld_auth_ref_clear(&auth);
    json_object_put(options);
    g_free(format);
    g_free(quality);
    g_free(bitrate);

    if (!queue_task(app, &task)) set_status(app, "Não foi possível adicionar a tarefa.");
    else set_status(app, "Download adicionado à fila.");
    dld_task_record_clear(&task);
}

static gboolean analyze_finished(gpointer data)
{
    UiUpdate *update = data;
    set_status(update->app, update->message);
    g_free(update->message);
    g_free(update);
    return G_SOURCE_REMOVE;
}

static void analyze_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    AnalyzeJob *job = data;
    DldMediaSummary summary;
    DldAppError error;
    dld_media_summary_init(&summary);
    dld_app_error_init(&error);
    const bool ok = dld_engine_analyze(&job->app->engine, job->url, job->playlist,
                                       job->has_auth ? &job->auth : NULL,
                                       &job->app->shutting_down, &summary, &error);
    GString *message = g_string_new(NULL);
    if (ok) {
        g_string_append_printf(message, "%s", summary.title != NULL ? summary.title : "Mídia encontrada");
        if (summary.is_playlist) g_string_append_printf(message, " — %zu itens", summary.playlist_entries);
        if (summary.has_duration) g_string_append_printf(message, " — %.0f s", summary.duration_seconds);
    } else {
        g_string_append(message, error.message != NULL ? error.message : "Falha ao analisar.");
    }
    UiUpdate *update = g_new0(UiUpdate, 1);
    update->app = job->app;
    update->message = g_string_free(message, FALSE);
    g_idle_add(analyze_finished, update);
    dld_media_summary_clear(&summary);
    dld_app_error_clear(&error);
    dld_auth_ref_clear(&job->auth);
    g_free(job->url);
    g_free(job);
}

static void analyze_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    DesktopApp *app = userdata;
    const char *url = gtk_editable_get_text(GTK_EDITABLE(app->download_url));
    if (!dld_validate_url(url)) {
        set_status(app, "Informe uma URL válida para analisar.");
        return;
    }
    AnalyzeJob *job = g_new0(AnalyzeJob, 1);
    job->app = app;
    job->url = g_strdup(url);
    job->playlist = gtk_check_button_get_active(app->download_playlist);
    dld_auth_ref_init(&job->auth);
    if (!auth_from_widgets(app, &job->auth, &job->has_auth)) {
        g_free(job->url);
        g_free(job);
        set_status(app, "Falha ao preparar autenticação.");
        return;
    }
    set_status(app, "Analisando…");
    g_thread_pool_push(app->analysis_pool, job, NULL);
}

static void convert_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    DesktopApp *app = userdata;
    const char *input = gtk_editable_get_text(GTK_EDITABLE(app->convert_input));
    if (input == NULL || *input == '\0') {
        set_status(app, "Escolha um arquivo para converter.");
        return;
    }
    char *format = gtk_combo_box_text_get_active_text(app->convert_format);
    char *acceleration = gtk_combo_box_text_get_active_text(app->convert_acceleration);
    struct json_object *options = json_object_new_object();
    json_object_object_add(options, "formato", json_object_new_string(format != NULL ? format : "mp4"));
    json_object_object_add(options, "aceleracao", json_object_new_string(acceleration != NULL ? acceleration : "auto"));

    DldTaskRecord task;
    dld_task_record_init(&task);
    task.id = make_task_id("convert");
    task.kind = DLD_TASK_CONVERT;
    task.input_path = dld_string_duplicate(input);
    task.options_json = dld_string_duplicate(json_object_to_json_string_ext(options, JSON_C_TO_STRING_PLAIN));
    const char *destination = gtk_editable_get_text(GTK_EDITABLE(app->convert_destination));
    if (destination != NULL && *destination != '\0') task.destination = dld_string_duplicate(destination);
    task.collision = DLD_COLLISION_RENAME;
    json_object_put(options);
    g_free(format);
    g_free(acceleration);
    if (!queue_task(app, &task)) set_status(app, "Não foi possível adicionar a conversão.");
    else set_status(app, "Conversão adicionada à fila.");
    dld_task_record_clear(&task);
}

static void file_chooser_response(GtkNativeDialog *dialog, int response, gpointer userdata)
{
    GtkEntry *entry = userdata;
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        if (file != NULL) {
            char *path = g_file_get_path(file);
            if (path != NULL) gtk_editable_set_text(GTK_EDITABLE(entry), path);
            g_free(path);
            g_object_unref(file);
        }
    }
    g_object_unref(dialog);
}

static void choose_file(GtkButton *button, gpointer userdata)
{
    (void)button;
    DesktopApp *app = userdata;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        "Escolher mídia", app->window, GTK_FILE_CHOOSER_ACTION_OPEN, "Abrir", "Cancelar");
    g_signal_connect(dialog, "response", G_CALLBACK(file_chooser_response), app->convert_input);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

static void folder_chooser_response(GtkNativeDialog *dialog, int response, gpointer userdata)
{
    GtkEntry *entry = userdata;
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        if (file != NULL) {
            char *path = g_file_get_path(file);
            if (path != NULL) gtk_editable_set_text(GTK_EDITABLE(entry), path);
            g_free(path);
            g_object_unref(file);
        }
    }
    g_object_unref(dialog);
}

static void choose_folder_for_entry(GtkButton *button, gpointer userdata)
{
    GtkEntry *entry = g_object_get_data(G_OBJECT(button), "target-entry");
    DesktopApp *app = userdata;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        "Escolher pasta", app->window, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Escolher", "Cancelar");
    g_signal_connect(dialog, "response", G_CALLBACK(folder_chooser_response), entry);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

static void protection_changed(GtkSwitch *widget, GParamSpec *pspec, gpointer userdata)
{
    (void)pspec;
    DesktopApp *app = userdata;
    app->engine.youtube_protection = gtk_switch_get_active(widget);
    const char *message = app->engine.youtube_protection
                              ? "Proteção do YouTube ativada."
                              : "Proteção do YouTube desativada.";
    set_status(app, message);
}

static void output_changed(GtkEditable *editable, gpointer userdata)
{
    DesktopApp *app = userdata;
    const char *text = gtk_editable_get_text(editable);
    if (text == NULL || *text == '\0') return;
    char *copy = dld_string_duplicate(text);
    if (copy == NULL) return;
    free(app->engine.output_dir);
    app->engine.output_dir = copy;

    if (app->sidebar_output_label != NULL) {
        gtk_label_set_text(app->sidebar_output_label, text);
    }
}

static void dependencies_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    DesktopApp *app = userdata;
    char *report = NULL;
    DldAppError error;
    dld_app_error_init(&error);
    if (dld_engine_check_dependencies(&app->engine, &report, &error)) set_status(app, "Dependências encontradas.");
    else set_status(app, error.message != NULL ? error.message : "Dependências ausentes.");
    free(report);
    dld_app_error_clear(&error);
}

static GtkWidget *make_page(const char *title_text, const char *subtitle_text)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(page, "page");

    GtkWidget *title = gtk_label_new(title_text);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_add_css_class(title, "page-title");
    gtk_box_append(GTK_BOX(page), title);

    GtkWidget *subtitle = gtk_label_new(subtitle_text);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
    gtk_widget_add_css_class(subtitle, "page-subtitle");
    gtk_box_append(GTK_BOX(page), subtitle);

    return page;
}

static GtkWidget *make_card(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(card, "card");
    return card;
}

static void append_section_title(GtkWidget *card, const char *number, const char *title_text)
{
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *number_label = gtk_label_new(number);
    gtk_widget_add_css_class(number_label, "section-number");

    GtkWidget *title = gtk_label_new(title_text);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_add_css_class(title, "section-title");

    gtk_box_append(GTK_BOX(header), number_label);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(card), header);
}

static GtkWidget *make_field(const char *label_text, GtkWidget *control)
{
    GtkWidget *field = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_add_css_class(label, "field-label");
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(field), label);
    gtk_box_append(GTK_BOX(field), control);
    return field;
}

static GtkWidget *wrap_page(GtkWidget *page)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

static GtkWidget *make_download_page(DesktopApp *app)
{
    GtkWidget *page = make_page(
        "Sua próxima descoberta.",
        "Baixe vídeos, músicas ou uma playlist inteira.");

    GtkWidget *link_card = make_card();
    append_section_title(link_card, "01", "Link do vídeo ou playlist");

    app->download_url = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(app->download_url, "Cole o link aqui…");
    gtk_box_append(GTK_BOX(link_card), GTK_WIDGET(app->download_url));

    app->download_playlist =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Baixar a playlist inteira"));
    gtk_box_append(GTK_BOX(link_card), GTK_WIDGET(app->download_playlist));
    gtk_box_append(GTK_BOX(page), link_card);

    GtkWidget *options_card = make_card();
    append_section_title(options_card, "02", "Como você quer salvar?");

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    gtk_widget_set_hexpand(grid, TRUE);

    app->download_type = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append(
        app->download_type,
        "video+audio",
        "Vídeo + áudio");
    gtk_combo_box_text_append(
        app->download_type,
        "video",
        "Somente vídeo");
    gtk_combo_box_text_append(
        app->download_type,
        "audio",
        "Somente áudio");
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(app->download_type),
        "video+audio");

    app->download_format = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *formats[] = {"auto", "mp4", "mkv", "webm", "mp3", "opus", "m4a", "flac", "wav"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        gtk_combo_box_text_append_text(app->download_format, formats[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->download_format), 0);

    app->download_quality = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *qualities[] = {"Sem limite", "480", "720", "1080", "1440", "2160"};
    for (size_t i = 0; i < 6U; ++i) {
        gtk_combo_box_text_append_text(app->download_quality, qualities[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->download_quality), 0);

    app->download_bitrate = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *bitrates[] = {"auto", "128K", "192K", "256K", "320K"};
    for (size_t i = 0; i < 5U; ++i) {
        gtk_combo_box_text_append_text(app->download_bitrate, bitrates[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->download_bitrate), 0);

    gtk_grid_attach(
        GTK_GRID(grid),
        make_field("Conteúdo", GTK_WIDGET(app->download_type)),
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(grid),
        make_field("Formato", GTK_WIDGET(app->download_format)),
        1, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(grid),
        make_field("Resolução", GTK_WIDGET(app->download_quality)),
        0, 1, 1, 1);
    gtk_grid_attach(
        GTK_GRID(grid),
        make_field("Taxa de bits", GTK_WIDGET(app->download_bitrate)),
        1, 1, 1, 1);

    gtk_box_append(GTK_BOX(options_card), grid);

    GtkWidget *benefits = gtk_label_new(
        "Capa automática  ·  Nome pelo título  ·  Evita duplicatas");
    gtk_label_set_xalign(GTK_LABEL(benefits), 0.0f);
    gtk_widget_add_css_class(benefits, "hint");
    gtk_box_append(GTK_BOX(options_card), benefits);

    GtkWidget *format_hint = gtk_label_new(
        "Opus e MP3 salvam somente áudio. A capa é incluída quando disponível.");
    gtk_label_set_xalign(GTK_LABEL(format_hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(format_hint), TRUE);
    gtk_widget_add_css_class(format_hint, "muted");
    gtk_box_append(GTK_BOX(options_card), format_hint);
    gtk_box_append(GTK_BOX(page), options_card);

    GtkWidget *access_card = make_card();
    append_section_title(access_card, "03", "Acesso, destino e cookies");

    GtkWidget *access_hint = gtk_label_new(
        "Use autenticação automática ou informe uma fonte específica quando o site exigir.");
    gtk_label_set_xalign(GTK_LABEL(access_hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(access_hint), TRUE);
    gtk_widget_add_css_class(access_hint, "muted");
    gtk_box_append(GTK_BOX(access_card), access_hint);

    GtkWidget *access_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(access_grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(access_grid), 12);
    gtk_widget_set_hexpand(access_grid, TRUE);

    app->auth_kind = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(app->auth_kind, "Automático / nenhum");
    gtk_combo_box_text_append_text(app->auth_kind, "cookies.txt Netscape");
    gtk_combo_box_text_append_text(app->auth_kind, "Perfil do navegador");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->auth_kind), 0);

    app->auth_value = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(
        app->auth_value,
        "/caminho/cookies.txt ou firefox:default");

    gtk_grid_attach(
        GTK_GRID(access_grid),
        make_field("Autenticação", GTK_WIDGET(app->auth_kind)),
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(access_grid),
        make_field("Referência", GTK_WIDGET(app->auth_value)),
        1, 0, 1, 1);

    GtkWidget *destination_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->download_destination = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(
        app->download_destination,
        "Padrão: pasta Downloads");

    GtkWidget *destination_button = gtk_button_new_with_label("Escolher…");
    gtk_widget_add_css_class(destination_button, "secondary");
    g_object_set_data(
        G_OBJECT(destination_button),
        "target-entry",
        app->download_destination);
    g_signal_connect(
        destination_button,
        "clicked",
        G_CALLBACK(choose_folder_for_entry),
        app);

    gtk_widget_set_hexpand(GTK_WIDGET(app->download_destination), TRUE);
    gtk_box_append(GTK_BOX(destination_box), GTK_WIDGET(app->download_destination));
    gtk_box_append(GTK_BOX(destination_box), destination_button);
    gtk_grid_attach(
        GTK_GRID(access_grid),
        make_field("Destino", destination_box),
        0, 1, 2, 1);

    gtk_box_append(GTK_BOX(access_card), access_grid);
    gtk_box_append(GTK_BOX(page), access_card);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *analyze = gtk_button_new_with_label("Consultar informações");
    gtk_widget_add_css_class(analyze, "secondary");
    g_signal_connect(analyze, "clicked", G_CALLBACK(analyze_clicked), app);

    GtkWidget *download = gtk_button_new_with_label("Adicionar à fila");
    gtk_widget_add_css_class(download, "primary");
    g_signal_connect(download, "clicked", G_CALLBACK(download_clicked), app);

    gtk_box_append(GTK_BOX(actions), analyze);
    gtk_box_append(GTK_BOX(actions), download);
    gtk_box_append(GTK_BOX(page), actions);

    return wrap_page(page);
}

static GtkWidget *make_convert_page(DesktopApp *app)
{
    GtkWidget *page = make_page(
        "Um arquivo. Novas possibilidades.",
        "Converta sua mídia no formato que combina com você.");

    GtkWidget *card = make_card();
    append_section_title(card, "01", "Arquivo e formato");

    GtkWidget *input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->convert_input = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(app->convert_input, "Caminho do arquivo de mídia");

    GtkWidget *browse = gtk_button_new_with_label("Escolher…");
    gtk_widget_add_css_class(browse, "secondary");
    g_signal_connect(browse, "clicked", G_CALLBACK(choose_file), app);

    gtk_widget_set_hexpand(GTK_WIDGET(app->convert_input), TRUE);
    gtk_box_append(GTK_BOX(input_box), GTK_WIDGET(app->convert_input));
    gtk_box_append(GTK_BOX(input_box), browse);
    gtk_box_append(GTK_BOX(card), make_field("Arquivo", input_box));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);

    app->convert_format = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *formats[] = {"mp4", "mkv", "webm", "mp3", "opus", "m4a", "flac", "wav"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        gtk_combo_box_text_append_text(app->convert_format, formats[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->convert_format), 0);

    app->convert_acceleration = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *modes[] = {"auto", "software", "vulkan", "vaapi", "amf", "cuda", "qsv"};
    for (size_t i = 0; i < 7U; ++i) {
        gtk_combo_box_text_append_text(app->convert_acceleration, modes[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->convert_acceleration), 0);

    gtk_grid_attach(
        GTK_GRID(grid),
        make_field("Formato", GTK_WIDGET(app->convert_format)),
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(grid),
        make_field("Aceleração", GTK_WIDGET(app->convert_acceleration)),
        1, 0, 1, 1);
    gtk_box_append(GTK_BOX(card), grid);

    GtkWidget *destination_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->convert_destination = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(
        app->convert_destination,
        "Padrão: pasta Downloads");

    GtkWidget *destination_button = gtk_button_new_with_label("Escolher…");
    gtk_widget_add_css_class(destination_button, "secondary");
    g_object_set_data(
        G_OBJECT(destination_button),
        "target-entry",
        app->convert_destination);
    g_signal_connect(
        destination_button,
        "clicked",
        G_CALLBACK(choose_folder_for_entry),
        app);

    gtk_widget_set_hexpand(GTK_WIDGET(app->convert_destination), TRUE);
    gtk_box_append(GTK_BOX(destination_box), GTK_WIDGET(app->convert_destination));
    gtk_box_append(GTK_BOX(destination_box), destination_button);
    gtk_box_append(GTK_BOX(card), make_field("Destino", destination_box));

    GtkWidget *convert = gtk_button_new_with_label("Iniciar conversão");
    gtk_widget_add_css_class(convert, "primary");
    g_signal_connect(convert, "clicked", G_CALLBACK(convert_clicked), app);
    gtk_box_append(GTK_BOX(card), convert);

    gtk_box_append(GTK_BOX(page), card);
    return wrap_page(page);
}

static GtkWidget *make_queue_page(DesktopApp *app)
{
    GtkWidget *page = make_page(
        "Cada faixa, no seu ritmo.",
        "Acompanhe transferências, conversões e arquivos concluídos.");

    app->queue_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(app->queue_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(app->queue_list), "queue-list");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(scroll),
        GTK_WIDGET(app->queue_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_append(GTK_BOX(page), scroll);
    return page;
}

static GtkWidget *make_settings_page(DesktopApp *app)
{
    GtkWidget *page = make_page(
        "Do seu jeito.",
        "Escolha onde sua biblioteca será salva.");

    GtkWidget *directory_card = make_card();
    append_section_title(directory_card, "01", "Pasta padrão");

    GtkWidget *destination_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->settings_output = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(
        GTK_EDITABLE(app->settings_output),
        app->engine.output_dir);
    g_signal_connect(
        app->settings_output,
        "changed",
        G_CALLBACK(output_changed),
        app);

    GtkWidget *browse = gtk_button_new_with_label("Escolher…");
    gtk_widget_add_css_class(browse, "secondary");
    g_object_set_data(
        G_OBJECT(browse),
        "target-entry",
        app->settings_output);
    g_signal_connect(
        browse,
        "clicked",
        G_CALLBACK(choose_folder_for_entry),
        app);

    gtk_widget_set_hexpand(GTK_WIDGET(app->settings_output), TRUE);
    gtk_box_append(GTK_BOX(destination_box), GTK_WIDGET(app->settings_output));
    gtk_box_append(GTK_BOX(destination_box), browse);
    gtk_box_append(
        GTK_BOX(directory_card),
        make_field("Diretório de saída", destination_box));
    gtk_box_append(GTK_BOX(page), directory_card);

    GtkWidget *youtube_card = make_card();
    append_section_title(youtube_card, "YT", "Proteção do YouTube");

    GtkWidget *protection_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *protection_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *protection_title =
        gtk_label_new("Ativar limite de segurança");
    gtk_label_set_xalign(GTK_LABEL(protection_title), 0.0f);
    gtk_widget_add_css_class(protection_title, "section-title");

    GtkWidget *protection_hint = gtk_label_new(
        "5 segundos entre downloads · até 300 vídeos em 90 minutos");
    gtk_label_set_xalign(GTK_LABEL(protection_hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(protection_hint), TRUE);
    gtk_widget_add_css_class(protection_hint, "muted");

    gtk_box_append(GTK_BOX(protection_text), protection_title);
    gtk_box_append(GTK_BOX(protection_text), protection_hint);
    gtk_widget_set_hexpand(protection_text, TRUE);

    app->settings_youtube_protection = GTK_SWITCH(gtk_switch_new());
    gtk_switch_set_active(
        app->settings_youtube_protection,
        app->engine.youtube_protection);
    g_signal_connect(
        app->settings_youtube_protection,
        "notify::active",
        G_CALLBACK(protection_changed),
        app);

    gtk_box_append(GTK_BOX(protection_row), protection_text);
    gtk_box_append(
        GTK_BOX(protection_row),
        GTK_WIDGET(app->settings_youtube_protection));
    gtk_box_append(GTK_BOX(youtube_card), protection_row);

    GtkWidget *retry_hint = gtk_label_new(
        "A contagem continua após reiniciar. Falhas temporárias usam novas tentativas com espera progressiva.");
    gtk_label_set_xalign(GTK_LABEL(retry_hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(retry_hint), TRUE);
    gtk_widget_add_css_class(retry_hint, "muted");
    gtk_box_append(GTK_BOX(youtube_card), retry_hint);
    gtk_box_append(GTK_BOX(page), youtube_card);

    GtkWidget *deps_card = make_card();
    append_section_title(deps_card, "03", "Ferramentas externas");

    GtkWidget *deps_hint = gtk_label_new(
        "Verifique yt-dlp, FFmpeg e ffprobe usados pelo Downloader.");
    gtk_label_set_xalign(GTK_LABEL(deps_hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(deps_hint), TRUE);
    gtk_widget_add_css_class(deps_hint, "muted");
    gtk_box_append(GTK_BOX(deps_card), deps_hint);

    GtkWidget *deps = gtk_button_new_with_label("Verificar dependências");
    gtk_widget_add_css_class(deps, "secondary");
    g_signal_connect(
        deps,
        "clicked",
        G_CALLBACK(dependencies_clicked),
        app);
    gtk_box_append(GTK_BOX(deps_card), deps);
    gtk_box_append(GTK_BOX(page), deps_card);

    return wrap_page(page);
}

static void load_history(DesktopApp *app)
{
    DldTaskRecord *tasks = NULL;
    size_t count = 0U;
    DldAppError error;
    dld_app_error_init(&error);

    if (dld_database_list_tasks(
            &app->engine.database,
            &tasks,
            &count,
            &error)) {
        for (size_t i = 0U; i < count; ++i) {
            add_queue_row(app, tasks[i].id, tasks[i].status);
        }
    }

    dld_database_free_task_list(tasks, count);
    dld_app_error_clear(&error);
}

static GtkWidget *make_sidebar(DesktopApp *app)
{
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar, "sidebar-shell");
    gtk_widget_set_size_request(sidebar, 220, -1);

    GtkWidget *brand = gtk_label_new("D / DOWNLOADER");
    gtk_label_set_xalign(GTK_LABEL(brand), 0.0f);
    gtk_widget_add_css_class(brand, "brand");
    gtk_box_append(GTK_BOX(sidebar), brand);

    GtkWidget *subtitle = gtk_label_new("Sua biblioteca de mídia");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_widget_add_css_class(subtitle, "brand-subtitle");
    gtk_widget_set_margin_top(subtitle, 4);
    gtk_widget_set_margin_bottom(subtitle, 28);
    gtk_box_append(GTK_BOX(sidebar), subtitle);

    GtkWidget *navigation = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(
        GTK_STACK_SIDEBAR(navigation),
        app->stack);
    gtk_widget_set_vexpand(navigation, TRUE);
    gtk_box_append(GTK_BOX(sidebar), navigation);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 18);
    gtk_widget_set_margin_bottom(separator, 12);
    gtk_box_append(GTK_BOX(sidebar), separator);

    GtkWidget *caption = gtk_label_new("SALVAR EM");
    gtk_label_set_xalign(GTK_LABEL(caption), 0.0f);
    gtk_widget_add_css_class(caption, "sidebar-caption");
    gtk_box_append(GTK_BOX(sidebar), caption);

    app->sidebar_output_label = GTK_LABEL(
        gtk_label_new(app->engine.output_dir));
    gtk_label_set_xalign(app->sidebar_output_label, 0.0f);
    gtk_label_set_wrap(app->sidebar_output_label, TRUE);
    gtk_widget_add_css_class(
        GTK_WIDGET(app->sidebar_output_label),
        "muted");
    gtk_widget_set_margin_top(
        GTK_WIDGET(app->sidebar_output_label),
        6);
    gtk_box_append(
        GTK_BOX(sidebar),
        GTK_WIDGET(app->sidebar_output_label));

    return sidebar;
}

static void activate(GtkApplication *application, gpointer userdata)
{
    DesktopApp *app = userdata;
    app->application = application;

    apply_theme();

    app->window = GTK_WINDOW(
        gtk_application_window_new(application));
    gtk_window_set_title(app->window, "Downloader");
    gtk_window_set_default_size(app->window, 1120, 760);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "app-root");

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    app->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(
        app->stack,
        GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(app->stack, 160);

    gtk_stack_add_titled(
        app->stack,
        make_download_page(app),
        "downloads",
        "Downloads");
    gtk_stack_add_titled(
        app->stack,
        make_convert_page(app),
        "converter",
        "Conversor");
    gtk_stack_add_titled(
        app->stack,
        make_queue_page(app),
        "queue",
        "Fila e histórico");
    gtk_stack_add_titled(
        app->stack,
        make_settings_page(app),
        "settings",
        "Configurações");

    gtk_widget_set_hexpand(GTK_WIDGET(app->stack), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(app->stack), TRUE);

    gtk_box_append(GTK_BOX(content), make_sidebar(app));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(app->stack));
    gtk_widget_set_vexpand(content, TRUE);

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(status_bar, "status-bar");

    GtkWidget *status_dot = gtk_label_new("●");
    gtk_widget_add_css_class(status_dot, "status-dot");
    gtk_box_append(GTK_BOX(status_bar), status_dot);

    app->status_label = GTK_LABEL(gtk_label_new(
        "Pronto. Escolha uma URL ou um arquivo para começar."));
    gtk_label_set_xalign(app->status_label, 0.0f);
    gtk_label_set_wrap(app->status_label, TRUE);
    gtk_widget_add_css_class(
        GTK_WIDGET(app->status_label),
        "muted");
    gtk_box_append(
        GTK_BOX(status_bar),
        GTK_WIDGET(app->status_label));

    gtk_box_append(GTK_BOX(root), content);
    gtk_box_append(GTK_BOX(root), status_bar);
    gtk_window_set_child(app->window, root);

    load_history(app);
    gtk_window_present(app->window);
}

int main(int argc, char **argv)
{
    DesktopApp app;
    memset(&app, 0, sizeof(app));
    dld_engine_init(&app.engine);
    DldEngineConfig config = dld_engine_config_default();
    DldAppError error;
    dld_app_error_init(&error);
    if (!dld_engine_open(&app.engine, &config, &error)) {
        fprintf(stderr, "erro: %s\n", error.message != NULL ? error.message : "falha ao iniciar");
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }
    dld_app_error_clear(&error);
    g_mutex_init(&app.jobs_mutex);
    app.cancel_flags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app.queue_rows = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        g_free,
        g_free);
    atomic_init(&app.shutting_down, false);
    app.download_pool = g_thread_pool_new(task_worker, &app, 2, FALSE, NULL);
    app.conversion_pool = g_thread_pool_new(task_worker, &app, 1, FALSE, NULL);
    app.analysis_pool = g_thread_pool_new(analyze_worker, NULL, 1, FALSE, NULL);

    GtkApplication *application = gtk_application_new("io.github.xoykor.Downloader", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    const int status = g_application_run(G_APPLICATION(application), argc, argv);

    /* Sinaliza todos os workers antes de destruir engine/widgets compartilhados. */
    atomic_store(&app.shutting_down, true);
    g_mutex_lock(&app.jobs_mutex);
    GHashTableIter cancel_iter;
    gpointer cancel_value = NULL;
    g_hash_table_iter_init(&cancel_iter, app.cancel_flags);
    while (g_hash_table_iter_next(&cancel_iter, NULL, &cancel_value)) {
        atomic_store((atomic_bool *)cancel_value, true);
    }
    g_mutex_unlock(&app.jobs_mutex);

    /* Espera os workers terminarem para não deixá-los acessar a engine já destruída. */
    g_thread_pool_free(app.download_pool, FALSE, TRUE);
    g_thread_pool_free(app.conversion_pool, FALSE, TRUE);
    g_thread_pool_free(app.analysis_pool, FALSE, TRUE);
    g_hash_table_destroy(app.cancel_flags);
    g_hash_table_destroy(app.queue_rows);
    g_mutex_clear(&app.jobs_mutex);
    dld_engine_clear(&app.engine);
    g_object_unref(application);
    return status;
}
