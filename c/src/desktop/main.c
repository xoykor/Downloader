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
    GHashTable *row_labels;   /* task id -> GtkLabel*; widgets pertencem ao GTK. */
    GMutex jobs_mutex;
};

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

/* Executado pelo main loop GTK, nunca pelo worker que produziu o evento. */
static gboolean apply_ui_update(gpointer data)
{
    UiUpdate *update = data;
    GtkLabel *label = g_hash_table_lookup(update->app->row_labels, update->task_id);
    if (label != NULL) {
        GString *text = g_string_new(update->task_id);
        g_string_append_printf(text, "  —  %s", status_label(update->status));
        if (update->has_progress) g_string_append_printf(text, "  %.0f%%", update->progress);
        if (update->speed != NULL && *update->speed != '\0') g_string_append_printf(text, "  %s", update->speed);
        if (update->message != NULL && *update->message != '\0') {
            g_string_append_printf(text, "  —  %s", update->message);
        }
        if (update->path != NULL && *update->path != '\0') g_string_append_printf(text, "  (%s)", update->path);
        gtk_label_set_text(label, text->str);
        g_string_free(text, TRUE);
    }
    if (update->message != NULL) set_status(update->app, update->message);
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
    UiUpdate *update = g_new0(UiUpdate, 1);
    update->app = app;
    update->task_id = g_strdup(event->task_id != NULL ? event->task_id : "tarefa");
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
        (void)dld_engine_execute_task(&app->engine, &job->task, job->cancelled,
                                      engine_event, app, &error);
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
    const char *task_id = g_object_get_data(G_OBJECT(button), "task-id");
    if (task_id == NULL) return;
    g_mutex_lock(&app->jobs_mutex);
    atomic_bool *flag = g_hash_table_lookup(app->cancel_flags, task_id);
    if (flag != NULL) atomic_store(flag, true);
    g_mutex_unlock(&app->jobs_mutex);
    set_status(app, flag != NULL ? "Cancelamento solicitado." : "A tarefa não está mais ativa.");
}

static void add_queue_row(DesktopApp *app, const char *task_id, DldTaskStatus status)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    char text[512];
    (void)snprintf(text, sizeof(text), "%s  —  %s", task_id, status_label(status));
    gtk_label_set_text(GTK_LABEL(label), text);
    GtkWidget *cancel = gtk_button_new_with_label("Cancelar");
    g_object_set_data_full(G_OBJECT(cancel), "task-id", g_strdup(task_id), g_free);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), app);
    gtk_box_append(GTK_BOX(row_box), label);
    gtk_box_append(GTK_BOX(row_box), cancel);
    gtk_list_box_append(app->queue_list, row_box);
    g_hash_table_replace(app->row_labels, g_strdup(task_id), label);
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

    char *type = gtk_combo_box_text_get_active_text(app->download_type);
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
    g_free(type);
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

static GtkWidget *labeled_row(const char *label_text, GtkWidget *control)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_size_request(label, 150, -1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), control);
    return box;
}

static GtkWidget *make_download_page(DesktopApp *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    GtkWidget *title = gtk_label_new("Downloads");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(box), title);

    app->download_url = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(app->download_url, "https://...");
    gtk_box_append(GTK_BOX(box), labeled_row("URL", GTK_WIDGET(app->download_url)));

    app->download_type = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *types[] = {"video+audio", "video", "audio"};
    for (size_t i = 0; i < 3U; ++i) {
        gtk_combo_box_text_append_text(app->download_type, types[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->download_type), 0);
    gtk_box_append(GTK_BOX(box), labeled_row("Tipo", GTK_WIDGET(app->download_type)));

    app->download_format = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *formats[] = {"auto", "mp4", "mkv", "webm", "mp3", "opus", "m4a", "flac", "wav"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        gtk_combo_box_text_append_text(app->download_format, formats[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->download_format), 0);
    gtk_box_append(GTK_BOX(box), labeled_row("Formato", GTK_WIDGET(app->download_format)));

    app->download_quality = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *qualities[] = {"Sem limite", "480", "720", "1080", "1440", "2160"};
    for (size_t i = 0; i < 6U; ++i) {
        gtk_combo_box_text_append_text(app->download_quality, qualities[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->download_quality), 0);
    gtk_box_append(GTK_BOX(box), labeled_row("Altura máxima", GTK_WIDGET(app->download_quality)));

    app->download_bitrate = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *bitrates[] = {"auto", "128K", "192K", "256K", "320K"};
    for (size_t i = 0; i < 5U; ++i) {
        gtk_combo_box_text_append_text(app->download_bitrate, bitrates[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->download_bitrate), 0);
    gtk_box_append(GTK_BOX(box), labeled_row("Bitrate", GTK_WIDGET(app->download_bitrate)));

    app->download_playlist = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Baixar playlist inteira"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->download_playlist));

    app->auth_kind = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(app->auth_kind, "Automático / nenhum");
    gtk_combo_box_text_append_text(app->auth_kind, "cookies.txt Netscape");
    gtk_combo_box_text_append_text(app->auth_kind, "Perfil do navegador");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->auth_kind), 0);
    gtk_box_append(GTK_BOX(box), labeled_row("Autenticação", GTK_WIDGET(app->auth_kind)));
    app->auth_value = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(app->auth_value, "/caminho/cookies.txt ou firefox:default");
    gtk_box_append(GTK_BOX(box), labeled_row("Referência", GTK_WIDGET(app->auth_value)));

    GtkWidget *dest_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->download_destination = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(app->download_destination, "Padrão: pasta Downloads");
    GtkWidget *dest_button = gtk_button_new_with_label("Escolher…");
    g_object_set_data(G_OBJECT(dest_button), "target-entry", app->download_destination);
    g_signal_connect(dest_button, "clicked", G_CALLBACK(choose_folder_for_entry), app);
    gtk_widget_set_hexpand(GTK_WIDGET(app->download_destination), TRUE);
    gtk_box_append(GTK_BOX(dest_box), GTK_WIDGET(app->download_destination));
    gtk_box_append(GTK_BOX(dest_box), dest_button);
    gtk_box_append(GTK_BOX(box), labeled_row("Destino", dest_box));

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *analyze = gtk_button_new_with_label("Analisar");
    GtkWidget *download = gtk_button_new_with_label("Adicionar à fila");
    gtk_widget_add_css_class(download, "suggested-action");
    g_signal_connect(analyze, "clicked", G_CALLBACK(analyze_clicked), app);
    g_signal_connect(download, "clicked", G_CALLBACK(download_clicked), app);
    gtk_box_append(GTK_BOX(actions), analyze);
    gtk_box_append(GTK_BOX(actions), download);
    gtk_box_append(GTK_BOX(box), actions);
    return box;
}

static GtkWidget *make_convert_page(DesktopApp *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    GtkWidget *title = gtk_label_new("Conversor");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->convert_input = GTK_ENTRY(gtk_entry_new());
    GtkWidget *browse = gtk_button_new_with_label("Escolher…");
    g_signal_connect(browse, "clicked", G_CALLBACK(choose_file), app);
    gtk_widget_set_hexpand(GTK_WIDGET(app->convert_input), TRUE);
    gtk_box_append(GTK_BOX(input_box), GTK_WIDGET(app->convert_input));
    gtk_box_append(GTK_BOX(input_box), browse);
    gtk_box_append(GTK_BOX(box), labeled_row("Arquivo", input_box));

    app->convert_format = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *formats[] = {"mp4", "mkv", "webm", "mp3", "opus", "m4a", "flac", "wav"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        gtk_combo_box_text_append_text(app->convert_format, formats[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->convert_format), 0);
    gtk_box_append(GTK_BOX(box), labeled_row("Formato", GTK_WIDGET(app->convert_format)));

    app->convert_acceleration = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *modes[] = {"auto", "software", "vulkan", "vaapi", "amf", "cuda", "qsv"};
    for (size_t i = 0; i < 7U; ++i) {
        gtk_combo_box_text_append_text(app->convert_acceleration, modes[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->convert_acceleration), 0);
    gtk_box_append(GTK_BOX(box), labeled_row("Aceleração", GTK_WIDGET(app->convert_acceleration)));

    GtkWidget *dest_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->convert_destination = GTK_ENTRY(gtk_entry_new());
    GtkWidget *dest_button = gtk_button_new_with_label("Escolher…");
    g_object_set_data(G_OBJECT(dest_button), "target-entry", app->convert_destination);
    g_signal_connect(dest_button, "clicked", G_CALLBACK(choose_folder_for_entry), app);
    gtk_widget_set_hexpand(GTK_WIDGET(app->convert_destination), TRUE);
    gtk_box_append(GTK_BOX(dest_box), GTK_WIDGET(app->convert_destination));
    gtk_box_append(GTK_BOX(dest_box), dest_button);
    gtk_box_append(GTK_BOX(box), labeled_row("Destino", dest_box));

    GtkWidget *convert = gtk_button_new_with_label("Adicionar conversão à fila");
    gtk_widget_add_css_class(convert, "suggested-action");
    g_signal_connect(convert, "clicked", G_CALLBACK(convert_clicked), app);
    gtk_box_append(GTK_BOX(box), convert);
    return box;
}

static GtkWidget *make_queue_page(DesktopApp *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    GtkWidget *title = gtk_label_new("Fila / Histórico");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(box), title);
    app->queue_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(app->queue_list, GTK_SELECTION_NONE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(app->queue_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);
    return box;
}

static GtkWidget *make_settings_page(DesktopApp *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    GtkWidget *title = gtk_label_new("Configurações");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *dest_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->settings_output = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(app->settings_output), app->engine.output_dir);
    g_signal_connect(app->settings_output, "changed", G_CALLBACK(output_changed), app);
    GtkWidget *browse = gtk_button_new_with_label("Escolher…");
    g_object_set_data(G_OBJECT(browse), "target-entry", app->settings_output);
    g_signal_connect(browse, "clicked", G_CALLBACK(choose_folder_for_entry), app);
    gtk_widget_set_hexpand(GTK_WIDGET(app->settings_output), TRUE);
    gtk_box_append(GTK_BOX(dest_box), GTK_WIDGET(app->settings_output));
    gtk_box_append(GTK_BOX(dest_box), browse);
    gtk_box_append(GTK_BOX(box), labeled_row("Pasta padrão", dest_box));

    app->settings_youtube_protection = GTK_SWITCH(gtk_switch_new());
    gtk_switch_set_active(app->settings_youtube_protection, app->engine.youtube_protection);
    g_signal_connect(app->settings_youtube_protection, "notify::active", G_CALLBACK(protection_changed), app);
    gtk_box_append(GTK_BOX(box), labeled_row("Proteção YouTube", GTK_WIDGET(app->settings_youtube_protection)));

    GtkWidget *deps = gtk_button_new_with_label("Verificar dependências");
    g_signal_connect(deps, "clicked", G_CALLBACK(dependencies_clicked), app);
    gtk_box_append(GTK_BOX(box), deps);
    return box;
}

static void load_history(DesktopApp *app)
{
    DldTaskRecord *tasks = NULL;
    size_t count = 0U;
    DldAppError error;
    dld_app_error_init(&error);
    if (dld_database_list_tasks(&app->engine.database, &tasks, &count, &error)) {
        for (size_t i = 0U; i < count; ++i) {
            add_queue_row(app, tasks[i].id, tasks[i].status);
        }
    }
    dld_database_free_task_list(tasks, count);
    dld_app_error_clear(&error);
}

static void activate(GtkApplication *application, gpointer userdata)
{
    DesktopApp *app = userdata;
    app->application = application;
    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(app->window, "Downloader");
    gtk_window_set_default_size(app->window, 980, 680);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(app->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    GtkWidget *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), app->stack);
    gtk_widget_set_size_request(sidebar, 190, -1);

    gtk_stack_add_titled(app->stack, make_download_page(app), "downloads", "Downloads");
    gtk_stack_add_titled(app->stack, make_convert_page(app), "converter", "Conversor");
    gtk_stack_add_titled(app->stack, make_queue_page(app), "queue", "Fila / Histórico");
    gtk_stack_add_titled(app->stack, make_settings_page(app), "settings", "Configurações");
    gtk_widget_set_hexpand(GTK_WIDGET(app->stack), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(app->stack), TRUE);
    gtk_box_append(GTK_BOX(content), sidebar);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(app->stack));

    app->status_label = GTK_LABEL(gtk_label_new("Pronto."));
    gtk_label_set_xalign(app->status_label, 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(app->status_label), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(app->status_label), 12);
    gtk_widget_set_margin_top(GTK_WIDGET(app->status_label), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(app->status_label), 8);
    gtk_box_append(GTK_BOX(root), content);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(app->status_label));
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
    app.row_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
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
    g_hash_table_destroy(app.row_labels);
    g_mutex_clear(&app.jobs_mutex);
    dld_engine_clear(&app.engine);
    g_object_unref(application);
    return status;
}
