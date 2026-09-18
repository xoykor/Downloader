/*
 * Orquestração de alto nível do Downloader.
 *
 * Regra de segurança mais importante: download/conversão nunca escreve o arquivo
 * final diretamente. O trabalho acontece em uma área temporária, passa por
 * ffprobe e somente então é publicado segundo a política de colisão escolhida.
 */

#include "downloader/engine.h"

#include "downloader/hardware.h"
#include "downloader/library.h"
#include "downloader/process.h"
#include "downloader/publish.h"

#include <dirent.h>
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0U;
    return ((uint64_t)now.tv_sec * UINT64_C(1000)) +
           ((uint64_t)now.tv_nsec / UINT64_C(1000000));
}

static bool ensure_directory(const char *path, DldAppError *error)
{
    if (path == NULL || *path == '\0') return false;
    char *copy = dld_string_duplicate(path);
    if (copy == NULL) return false;
    for (char *p = copy + 1; *p != '\0'; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) goto fail;
        *p = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) goto fail;
    free(copy);
    return true;
fail:
    (void)dld_app_error_set(error, DLD_ERROR_PERMISSION,
                            "Não foi possível criar diretório de trabalho.",
                            "inicialização", true, errno);
    free(copy);
    return false;
}

static char *path_join(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return NULL;
    const bool slash = *a != '\0' && a[strlen(a) - 1U] == '/';
    const size_t length = strlen(a) + strlen(b) + (slash ? 1U : 2U);
    char *path = malloc(length);
    if (path != NULL) (void)snprintf(path, length, "%s%s%s", a, slash ? "" : "/", b);
    return path;
}

static const char *basename_ptr(const char *path)
{
    const char *slash = path == NULL ? NULL : strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static char *stem_copy(const char *path)
{
    const char *base = basename_ptr(path);
    if (base == NULL) return NULL;
    const char *dot = strrchr(base, '.');
    const size_t length = dot == NULL ? strlen(base) : (size_t)(dot - base);
    char *stem = malloc(length + 1U);
    if (stem == NULL) return NULL;
    memcpy(stem, base, length);
    stem[length] = '\0';
    return stem;
}

static bool regular_file(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void emit_event(DldEngineEventCallback callback,
                       void *userdata,
                       const char *task_id,
                       DldTaskStatus status,
                       bool has_progress,
                       double progress,
                       const char *speed,
                       const char *message,
                       const char *path)
{
    if (callback == NULL) return;

    const DldEngineEvent event = {
        .task_id = task_id,
        .status = status,
        .has_progress = has_progress,
        .progress_percent = progress,
        .speed = speed,
        .message = message,
        .path = path,
    };
    callback(&event, userdata);
}

static void emit(DldEngineEventCallback callback,
                 void *userdata,
                 const DldTaskRecord *task,
                 bool has_progress,
                 double progress,
                 const char *speed,
                 const char *message,
                 const char *path)
{
    emit_event(
        callback,
        userdata,
        task != NULL ? task->id : NULL,
        task != NULL ? task->status : DLD_STATUS_FAILED,
        has_progress,
        progress,
        speed,
        message,
        path);
}

DldEngineConfig dld_engine_config_default(void)
{
    const char *home = getenv("HOME");
    static char data[PATH_MAX];
    static char output[PATH_MAX];
    const char *override = getenv("DOWNLOADER_DATA_DIR");
    if (override != NULL && *override != '\0') {
        (void)snprintf(data, sizeof(data), "%s", override);
    } else {
        (void)snprintf(data, sizeof(data), "%s/.downloader", home != NULL ? home : ".");
    }
    (void)snprintf(output, sizeof(output), "%s/Downloads", home != NULL ? home : ".");
    return (DldEngineConfig){
        .data_dir = data,
        .output_dir = output,
        .yt_dlp = "yt-dlp",
        .ffmpeg = "ffmpeg",
        .ffprobe = "ffprobe",
        .youtube_protection = true,
    };
}

void dld_engine_init(DldEngine *engine)
{
    if (engine == NULL) return;
    memset(engine, 0, sizeof(*engine));
    dld_database_init(&engine->database);
}

void dld_engine_clear(DldEngine *engine)
{
    if (engine == NULL) return;
    dld_database_close(&engine->database);
    free(engine->data_dir);
    free(engine->output_dir);
    free(engine->yt_dlp);
    free(engine->ffmpeg);
    free(engine->ffprobe);
    dld_engine_init(engine);
}

bool dld_engine_open(DldEngine *engine, const DldEngineConfig *config, DldAppError *error)
{
    if (engine == NULL || config == NULL) return false;
    dld_engine_clear(engine);
    engine->data_dir = dld_string_duplicate(config->data_dir);
    engine->output_dir = dld_string_duplicate(config->output_dir);
    engine->yt_dlp = dld_string_duplicate(config->yt_dlp != NULL ? config->yt_dlp : "yt-dlp");
    engine->ffmpeg = dld_string_duplicate(config->ffmpeg != NULL ? config->ffmpeg : "ffmpeg");
    engine->ffprobe = dld_string_duplicate(config->ffprobe != NULL ? config->ffprobe : "ffprobe");
    engine->youtube_protection = config->youtube_protection;
    if (engine->data_dir == NULL || engine->output_dir == NULL || engine->yt_dlp == NULL ||
        engine->ffmpeg == NULL || engine->ffprobe == NULL) goto oom;
    if (!ensure_directory(engine->data_dir, error) || !ensure_directory(engine->output_dir, error)) return false;
    char *db_path = path_join(engine->data_dir, "tasks.sqlite");
    if (db_path == NULL) goto oom;
    const bool opened = dld_database_open(&engine->database, db_path, error);
    free(db_path);
    return opened;
oom:
    (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Memória insuficiente para abrir motor.",
                            "inicialização", false, 0);
    return false;
}

bool dld_engine_check_dependencies(DldEngine *engine, char **report, DldAppError *error)
{
    if (report != NULL) *report = NULL;
    if (engine == NULL || report == NULL) return false;
    const char *names[] = {engine->yt_dlp, engine->ffmpeg, engine->ffprobe};
    char *paths[3] = {NULL, NULL, NULL};
    char *versions[3] = {NULL, NULL, NULL};
    bool ok = true;
    size_t length = 1U;
    for (size_t i = 0U; i < 3U; ++i) {
        if (!dld_process_detect(names[i], &paths[i], &versions[i])) ok = false;
        length += strlen(names[i]) + (paths[i] != NULL ? strlen(paths[i]) : 8U) +
                  (versions[i] != NULL ? strlen(versions[i]) : 0U) + 16U;
    }
    char *text = malloc(length);
    if (text == NULL) ok = false;
    if (text != NULL) {
        text[0] = '\0';
        for (size_t i = 0U; i < 3U; ++i) {
            char line[2048];
            (void)snprintf(line, sizeof(line), "%s: %s%s%s\n", names[i],
                           paths[i] != NULL ? paths[i] : "ausente",
                           versions[i] != NULL ? " — " : "",
                           versions[i] != NULL ? versions[i] : "");
            strncat(text, line, length - strlen(text) - 1U);
        }
        *report = text;
    }
    for (size_t i = 0U; i < 3U; ++i) {
        free(paths[i]);
        free(versions[i]);
    }
    if (!ok) {
        (void)dld_app_error_set(error, DLD_ERROR_DEPENDENCY,
                                "yt-dlp, ffmpeg e ffprobe precisam estar disponíveis no PATH.",
                                "dependências", false, 0);
    }
    return ok;
}

static bool run_command(const DldCommand *command, unsigned timeout_ms, atomic_bool *cancel_flag,
                        DldProcessLineCallback callback, void *userdata,
                        DldProcessResult *result, DldAppError *error)
{
    if (command == NULL || command->argc == 0U) return false;
    DldProcessSpec spec = {
        .program = command->argv[0],
        .argv = command->argv,
        .working_directory = NULL,
        .timeout_ms = timeout_ms,
    };
    return dld_process_run(&spec, cancel_flag, callback, userdata, result, error);
}

bool dld_engine_analyze(DldEngine *engine, const char *url, bool playlist,
                        const DldAuthRef *auth, atomic_bool *cancel_flag,
                        DldMediaSummary *summary, DldAppError *error)
{
    if (engine == NULL || summary == NULL) return false;
    DldCommand command;
    DldProcessResult result;
    dld_command_init(&command);
    dld_process_result_init(&result);
    bool ok = dld_build_analysis_command(engine->yt_dlp, url, playlist, auth, &command, error) &&
              run_command(&command, 120000U, cancel_flag, NULL, NULL, &result, error);
    if (ok && result.cancelled) {
        (void)dld_app_error_set(error, DLD_ERROR_CANCELLED, "Análise cancelada.", "análise", false, 0);
        ok = false;
    } else if (ok && result.timed_out) {
        (void)dld_app_error_set(error, DLD_ERROR_TIMEOUT, "Análise excedeu o tempo limite.", "análise", false, 0);
        ok = false;
    } else if (ok && result.exit_code != 0) {
        (void)dld_app_error_set(error, DLD_ERROR_NETWORK,
                                result.stderr_text != NULL && *result.stderr_text != '\0' ?
                                result.stderr_text : "yt-dlp falhou ao analisar URL.",
                                "análise", true, result.exit_code);
        ok = false;
    }
    if (ok) ok = dld_parse_ytdlp_summary(result.stdout_text, summary, error);
    dld_command_clear(&command);
    dld_process_result_clear(&result);
    return ok;
}

static bool is_youtube_url(const char *url)
{
    return url != NULL && (strstr(url, "youtube.com") != NULL || strstr(url, "youtu.be") != NULL);
}

static bool cancel_requested(atomic_bool *flag)
{
    return flag != NULL && atomic_load(flag);
}

static bool youtube_remaining_allowance(DldEngine *engine,
                                        unsigned *allowance,
                                        DldAppError *error)
{
    if (allowance == NULL) return false;
    *allowance = 300U;

    if (!engine->youtube_protection) return true;

    const uint64_t window_ms = UINT64_C(90) * 60U * 1000U;
    const uint64_t now = now_ms();
    const uint64_t since = now > window_ms ? now - window_ms : 0U;

    if (!dld_database_prune_youtube_starts(
            &engine->database,
            since,
            error)) {
        return false;
    }

    unsigned count = 0U;
    uint64_t oldest = 0U;
    if (!dld_database_count_youtube_starts_since(
            &engine->database,
            since,
            &count,
            &oldest,
            error)) {
        return false;
    }

    (void)oldest;
    *allowance = count >= 300U ? 0U : 300U - count;
    return true;
}

static bool probe_file(DldEngine *engine, const char *path, DldProbeSummary *summary,
                       DldAppError *error)
{
    DldCommand command;
    DldProcessResult result;
    dld_command_init(&command);
    dld_process_result_init(&result);
    bool ok = dld_build_ffprobe_command(engine->ffprobe, path, &command, error) &&
              run_command(&command, 30000U, NULL, NULL, NULL, &result, error);
    if (ok && result.exit_code != 0) {
        (void)dld_app_error_set(error, DLD_ERROR_INVALID_MEDIA,
                                "ffprobe rejeitou o arquivo produzido.", "validação",
                                true, result.exit_code);
        ok = false;
    }
    if (ok) ok = dld_parse_ffprobe_summary(result.stdout_text, summary, error);
    dld_command_clear(&command);
    dld_process_result_clear(&result);
    return ok;
}

static bool is_auxiliary_download_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return true;
    static const char *ignored[] = {".part", ".ytdl", ".jpg", ".jpeg", ".png", ".webp", ".json", ".description"};
    for (size_t i = 0U; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        if (strcasecmp(dot, ignored[i]) == 0) return true;
    }
    return false;
}

static char *extract_id_from_filename(const char *name)
{
    const char *close = strrchr(name, ']');
    if (close == NULL) return NULL;
    const char *open = close;
    while (open > name && *open != '[') --open;
    if (*open != '[' || open + 1 >= close) return NULL;
    const size_t length = (size_t)(close - open - 1);
    char *id = malloc(length + 1U);
    if (id == NULL) return NULL;
    memcpy(id, open + 1, length);
    id[length] = '\0';
    return id;
}

typedef struct {
    DldEngine *engine;
    const DldTaskRecord *task;
    DldEngineEventCallback callback;
    void *userdata;
    bool account_youtube_starts;

    /*
     * Publicação incremental da playlist. Cada FILE do yt-dlp é validado e
     * movido imediatamente para o destino final, sem esperar a playlist acabar.
     */
    const char *destination_dir;
    const char *format;
    size_t published;
    size_t failed_items;
    char *last_path;
} ProgressContext;

static char *track_event_id(const DldTaskRecord *task,
                            const char *media_id,
                            size_t playlist_index)
{
    if (task == NULL || task->id == NULL) return NULL;

    char fallback[64];
    const char *suffix = media_id;
    if (suffix == NULL || *suffix == '\0') {
        (void)snprintf(
            fallback,
            sizeof(fallback),
            "faixa-%zu",
            playlist_index > 0U ? playlist_index : 1U);
        suffix = fallback;
    }

    const size_t length = strlen(task->id) + strlen(suffix) + 3U;
    char *id = malloc(length);
    if (id != NULL) {
        (void)snprintf(id, length, "%s::%s", task->id, suffix);
    }
    return id;
}

static void track_display_message(const DldTrackLine *track,
                                  char *buffer,
                                  size_t buffer_size)
{
    const char *title =
        track->title[0] != '\0' ? track->title : "Faixa";

    if (track->playlist_index > 0U && track->playlist_count > 0U) {
        (void)snprintf(
            buffer,
            buffer_size,
            "%zu/%zu · %s",
            track->playlist_index,
            track->playlist_count,
            title);
    } else {
        (void)snprintf(buffer, buffer_size, "%s", title);
    }
}

static bool publish_finished_track(ProgressContext *context,
                                  const DldTrackLine *track,
                                  const char *event_id,
                                  const char *message)
{
    if (track->filepath[0] == '\0' || !regular_file(track->filepath)) {
        return false;
    }

    /*
     * A linha FILE só chega depois do pós-processamento do yt-dlp. Nesse ponto
     * já é seguro validar e retirar a faixa do staging enquanto o yt-dlp segue
     * para o próximo item da playlist.
     */
    emit_event(
        context->callback,
        context->userdata,
        event_id,
        DLD_STATUS_VALIDATING,
        true,
        100.0,
        NULL,
        message,
        NULL);

    DldAppError item_error;
    dld_app_error_init(&item_error);

    DldProbeSummary probe;
    dld_probe_summary_init(&probe);
    if (!probe_file(
            context->engine,
            track->filepath,
            &probe,
            &item_error)) {
        goto fail;
    }
    dld_probe_summary_clear(&probe);

    const char *filename = basename_ptr(track->filepath);
    if (filename == NULL || *filename == '\0') {
        (void)dld_app_error_set(
            &item_error,
            DLD_ERROR_INVALID_MEDIA,
            "Arquivo concluído sem nome válido.",
            "publicação",
            false,
            0);
        goto fail_without_probe;
    }

    char *destination = path_join(
        context->destination_dir,
        filename);
    if (destination == NULL) {
        (void)dld_app_error_set(
            &item_error,
            DLD_ERROR_INTERNAL,
            "Memória insuficiente para publicar a faixa.",
            "publicação",
            false,
            0);
        goto fail_without_probe;
    }

    DldPublishedOutput output;
    dld_published_output_init(&output);
    if (!dld_publish_output(
            track->filepath,
            destination,
            context->task->collision,
            &output,
            &item_error)) {
        dld_published_output_clear(&output);
        free(destination);
        goto fail_without_probe;
    }

    if (track->id[0] != '\0') {
        DldAppError index_error;
        dld_app_error_init(&index_error);
        (void)dld_library_record(
            context->destination_dir,
            track->id,
            context->format,
            output.path,
            &index_error);
        dld_app_error_clear(&index_error);
    }

    free(context->last_path);
    context->last_path = dld_string_duplicate(output.path);
    ++context->published;

    emit_event(
        context->callback,
        context->userdata,
        event_id,
        DLD_STATUS_COMPLETED,
        true,
        100.0,
        NULL,
        message,
        output.path);

    dld_published_output_clear(&output);
    free(destination);
    dld_app_error_clear(&item_error);
    return true;

fail:
    dld_probe_summary_clear(&probe);

fail_without_probe:
    ++context->failed_items;
    emit_event(
        context->callback,
        context->userdata,
        event_id,
        DLD_STATUS_FAILED,
        false,
        0.0,
        NULL,
        item_error.message != NULL ? item_error.message : message,
        NULL);
    dld_app_error_clear(&item_error);
    return false;
}

static void download_progress_line(const char *line, void *userdata)
{
    ProgressContext *context = userdata;

    if (context->account_youtube_starts &&
        line != NULL &&
        strncmp(line, "POLICY_VIDEO ", 13U) == 0) {
        /*
         * Contabiliza somente quando yt-dlp realmente entra em before_dl.
         * Erro de persistência não derruba a mídia; ele só desativa a precisão
         * do histórico desta execução.
         */
        DldAppError counter_error;
        dld_app_error_init(&counter_error);
        if (!dld_database_record_youtube_start(
                &context->engine->database,
                now_ms(),
                &counter_error)) {
            context->account_youtube_starts = false;
        }
        dld_app_error_clear(&counter_error);
        return;
    }

    DldTrackLine track;
    if (!dld_parse_track_line(line, &track)) return;

    char *event_id = track_event_id(
        context->task,
        track.id,
        track.playlist_index);
    if (event_id == NULL) return;

    char message[640];
    track_display_message(&track, message, sizeof(message));

    if (track.kind == DLD_TRACK_LINE_PROGRESS) {
        emit_event(
            context->callback,
            context->userdata,
            event_id,
            DLD_STATUS_DOWNLOADING,
            track.progress.has_percent,
            track.progress.percent,
            track.progress.speed[0] != '\0' ? track.progress.speed : NULL,
            message,
            NULL);
    } else if (track.kind == DLD_TRACK_LINE_FILE) {
        /*
         * Publica agora. O caminho temporário nunca aparece como se fosse o
         * destino do usuário; o evento final já aponta para Downloads (ou para a
         * pasta escolhida na tarefa).
         */
        (void)publish_finished_track(
            context,
            &track,
            event_id,
            message);
    }

    free(event_id);
}

static bool set_task_destination(DldTaskRecord *task, const char *path)
{
    char *copy = dld_string_duplicate(path);
    if (path != NULL && copy == NULL) return false;
    free(task->destination);
    task->destination = copy;
    return true;
}

/*
 * Pipeline de download: analisar -> deduplicar -> limitar -> baixar em temporário
 * -> validar cada mídia -> publicar -> registrar no índice local.
 */
static bool execute_download(DldEngine *engine, DldTaskRecord *task, atomic_bool *cancel_flag,
                             DldEngineEventCallback callback, void *userdata, DldAppError *error)
{
    if (task->input_url == NULL) {
        (void)dld_app_error_set(error, DLD_ERROR_INVALID_URL, "Tarefa sem URL.", "download", false, 0);
        return false;
    }
    const bool playlist = dld_json_get_bool(task->options_json, "playlist", false);
    char *format = dld_json_get_string_copy(task->options_json, "output_format", "auto");
    char *kind = dld_json_get_string_copy(task->options_json, "media_kind", "video+audio");
    char *bitrate = dld_json_get_string_copy(task->options_json, "bitrate", "auto");
    int max_height_value = dld_json_get_int(task->options_json, "max_height", 0);
    if (max_height_value < 0) max_height_value = 0;
    const unsigned max_height = (unsigned)max_height_value;
    if (format == NULL || kind == NULL || bitrate == NULL) goto oom;

    DldMediaSummary summary;
    dld_media_summary_init(&summary);
    const DldAuthRef *auth = task->has_auth ? &task->auth : NULL;

    emit_event(
        callback,
        userdata,
        task->id,
        DLD_STATUS_ANALYZING,
        false,
        0.0,
        NULL,
        playlist ? "Analisando playlist…" : "Analisando mídia…",
        NULL);

    if (!dld_engine_analyze(engine, task->input_url, playlist, auth, cancel_flag, &summary, error)) {
        dld_media_summary_clear(&summary);
        goto fail;
    }

    const char *destination_dir = task->destination != NULL ? task->destination : engine->output_dir;
    if (!ensure_directory(destination_dir, error)) {
        dld_media_summary_clear(&summary);
        goto fail;
    }

    if (!playlist && summary.id != NULL) {
        char *existing = NULL;
        if (dld_library_find(destination_dir, summary.id, format, &existing, error)) {
            DldProbeSummary probe;
            dld_probe_summary_init(&probe);
            if (probe_file(engine, existing, &probe, error)) {
                (void)set_task_destination(task, existing);
                emit(callback, userdata, task, true, 100.0, NULL,
                     "Já existe — download pulado", existing);
                dld_probe_summary_clear(&probe);
                free(existing);
                dld_media_summary_clear(&summary);
                free(format);
                free(kind);
                free(bitrate);
                return true;
            }
            dld_probe_summary_clear(&probe);
            free(existing);
            dld_app_error_clear(error);
        }
    }

    const bool youtube_limited =
        engine->youtube_protection &&
        is_youtube_url(task->input_url);
    unsigned youtube_allowance = 0U;

    if (youtube_limited) {
        if (!youtube_remaining_allowance(
                engine,
                &youtube_allowance,
                error)) {
            dld_media_summary_clear(&summary);
            goto fail;
        }

        if (youtube_allowance == 0U) {
            (void)dld_app_error_set(
                error,
                DLD_ERROR_NETWORK,
                "Limite local do YouTube atingido: 300 vídeos em 90 minutos. "
                "Nenhum download foi iniciado nesta tentativa.",
                "limite YouTube",
                false,
                0);
            dld_media_summary_clear(&summary);
            goto fail;
        }
    }

    char starting_message[192];
    if (playlist && summary.playlist_entries > 0U) {
        (void)snprintf(
            starting_message,
            sizeof(starting_message),
            "Playlist encontrada: %zu itens · iniciando yt-dlp…",
            summary.playlist_entries);
    } else {
        (void)snprintf(
            starting_message,
            sizeof(starting_message),
            "Iniciando yt-dlp…");
    }
    emit_event(
        callback,
        userdata,
        task->id,
        DLD_STATUS_DOWNLOADING,
        false,
        0.0,
        NULL,
        starting_message,
        NULL);

    char *tmp_root = path_join(engine->data_dir, "tmp");
    char *tmp_dir = tmp_root != NULL ? path_join(tmp_root, task->id) : NULL;
    free(tmp_root);
    if (tmp_dir == NULL || !ensure_directory(tmp_dir, error)) {
        free(tmp_dir);
        dld_media_summary_clear(&summary);
        goto fail;
    }
    char *template_path = path_join(tmp_dir, "%(title)s [%(id)s].%(ext)s");
    if (template_path == NULL) {
        free(tmp_dir);
        dld_media_summary_clear(&summary);
        goto oom;
    }

    DldCommand command;
    DldProcessResult result;
    dld_command_init(&command);
    dld_process_result_init(&result);
    bool ok = dld_build_download_command(
        engine->yt_dlp,
        task->input_url,
        template_path,
        playlist,
        kind,
        format,
        max_height,
        bitrate,
        youtube_limited,
        youtube_allowance,
        auth,
        &command,
        error);

    ProgressContext progress_context = {
        .engine = engine,
        .task = task,
        .callback = callback,
        .userdata = userdata,
        .account_youtube_starts = youtube_limited,
        .destination_dir = destination_dir,
        .format = format,
        .published = 0U,
        .failed_items = 0U,
        .last_path = NULL,
    };

    if (ok) {
        ok = run_command(
            &command,
            0U,
            cancel_flag,
            download_progress_line,
            &progress_context,
            &result,
            error);
    }

    const bool process_cancelled =
        ok && (result.cancelled || cancel_requested(cancel_flag));
    const int process_exit_code = ok ? result.exit_code : -1;
    char *process_error = NULL;

    if (ok && result.stderr_text != NULL && *result.stderr_text != '\0') {
        process_error = dld_string_duplicate(result.stderr_text);
    }

    if (process_cancelled) {
        (void)dld_app_error_set(
            error,
            DLD_ERROR_CANCELLED,
            "Download cancelado.",
            "download",
            false,
            0);
        ok = false;
    }

    dld_command_clear(&command);
    dld_process_result_clear(&result);
    free(template_path);

    if (!ok) {
        free(progress_context.last_path);
        free(process_error);
        dld_media_summary_clear(&summary);
        free(tmp_dir);
        goto fail;
    }

    /*
     * Não abortamos só porque o yt-dlp terminou com código diferente de zero.
     * Em playlists isso pode significar que UMA faixa foi removida, enquanto
     * várias outras já estão completas no staging e devem ser publicadas.
     */

    DIR *directory = opendir(tmp_dir);
    if (directory == NULL) {
        (void)dld_app_error_set(error, DLD_ERROR_INVALID_MEDIA,
                                "Download terminou sem arquivos de saída.", "download", true, errno);
        dld_media_summary_clear(&summary);
        free(tmp_dir);
        free(process_error);
        goto fail;
    }
    /*
     * Normalmente os arquivos já foram publicados pelos eventos FILE. Esta
     * varredura fica como fallback para versões/formatos do yt-dlp que não
     * emitirem o evento after_move esperado.
     */
    size_t published = progress_context.published;
    size_t failed_items = progress_context.failed_items;
    char *last_path = progress_context.last_path;
    progress_context.last_path = NULL;
    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' ||
            is_auxiliary_download_file(entry->d_name)) {
            continue;
        }

        char *source = path_join(tmp_dir, entry->d_name);
        if (source == NULL || !regular_file(source)) {
            free(source);
            continue;
        }

        char *media_id = extract_id_from_filename(entry->d_name);
        char *child_id = track_event_id(task, media_id, published + failed_items + 1U);
        char *display_name = stem_copy(entry->d_name);

        if (display_name != NULL && media_id != NULL) {
            char suffix[256];
            (void)snprintf(suffix, sizeof(suffix), " [%s]", media_id);
            const size_t name_length = strlen(display_name);
            const size_t suffix_length = strlen(suffix);
            if (name_length >= suffix_length &&
                strcmp(display_name + name_length - suffix_length, suffix) == 0) {
                display_name[name_length - suffix_length] = '\0';
            }
        }

        DldProbeSummary probe;
        dld_probe_summary_init(&probe);
        if (!probe_file(engine, source, &probe, error)) {
            dld_probe_summary_clear(&probe);

            if (playlist) {
                emit_event(
                    callback,
                    userdata,
                    child_id != NULL ? child_id : task->id,
                    DLD_STATUS_FAILED,
                    false,
                    0.0,
                    NULL,
                    display_name != NULL ? display_name : "Faixa inválida",
                    NULL);
                ++failed_items;
                dld_app_error_clear(error);
                free(display_name);
                free(child_id);
                free(media_id);
                free(source);
                continue;
            }

            free(display_name);
            free(child_id);
            free(media_id);
            free(source);
            closedir(directory);
            dld_media_summary_clear(&summary);
            free(tmp_dir);
            free(last_path);
            free(process_error);
            goto fail;
        }
        dld_probe_summary_clear(&probe);

        char *destination = path_join(destination_dir, entry->d_name);
        if (destination == NULL) {
            free(display_name);
            free(child_id);
            free(media_id);
            free(source);
            continue;
        }

        DldPublishedOutput output;
        dld_published_output_init(&output);

        if (!dld_publish_output(
                source,
                destination,
                task->collision,
                &output,
                error)) {
            dld_published_output_clear(&output);

            if (playlist) {
                emit_event(
                    callback,
                    userdata,
                    child_id != NULL ? child_id : task->id,
                    DLD_STATUS_FAILED,
                    false,
                    0.0,
                    NULL,
                    display_name != NULL ? display_name : "Falha ao publicar faixa",
                    NULL);
                ++failed_items;
                dld_app_error_clear(error);
                free(destination);
                free(display_name);
                free(child_id);
                free(media_id);
                free(source);
                continue;
            }

            free(destination);
            free(display_name);
            free(child_id);
            free(media_id);
            free(source);
            closedir(directory);
            dld_media_summary_clear(&summary);
            free(tmp_dir);
            free(last_path);
            free(process_error);
            goto fail;
        }

        if (media_id != NULL) {
            (void)dld_library_record(
                destination_dir,
                media_id,
                format,
                output.path,
                error);
            dld_app_error_clear(error);
        }

        free(last_path);
        last_path = dld_string_duplicate(output.path);
        ++published;

        emit_event(
            callback,
            userdata,
            child_id != NULL ? child_id : task->id,
            DLD_STATUS_COMPLETED,
            true,
            100.0,
            NULL,
            display_name != NULL ? display_name : "Faixa concluída",
            output.path);

        dld_published_output_clear(&output);
        free(destination);
        free(display_name);
        free(child_id);
        free(media_id);
        free(source);
    }

    closedir(directory);
    (void)rmdir(tmp_dir);
    free(tmp_dir);
    dld_media_summary_clear(&summary);

    if (published == 0U) {
        free(last_path);

        if (process_exit_code != 0 && process_error != NULL) {
            (void)dld_app_error_set(
                error,
                DLD_ERROR_NETWORK,
                process_error,
                "download",
                true,
                process_exit_code);
        } else {
            (void)dld_app_error_set(
                error,
                DLD_ERROR_INVALID_MEDIA,
                "Nenhum arquivo de mídia válido foi produzido.",
                "download",
                false,
                0);
        }

        free(process_error);
        goto fail;
    }

    if (playlist && (failed_items > 0U || process_exit_code != 0)) {
        emit(
            callback,
            userdata,
            task,
            false,
            0.0,
            NULL,
            "Playlist concluída parcialmente; itens disponíveis foram salvos.",
            destination_dir);
    }

    if (!playlist && last_path != NULL) {
        (void)set_task_destination(task, last_path);
    }

    free(process_error);
    free(last_path);
    free(format);
    free(kind);
    free(bitrate);
    return true;

oom:
    (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Memória insuficiente.", "download", false, 0);
fail:
    free(format);
    free(kind);
    free(bitrate);
    return false;
}

static DldAccelerationMode parse_acceleration(const char *text)
{
    if (text == NULL || strcmp(text, "auto") == 0) return DLD_ACCELERATION_AUTO;
    if (strcmp(text, "software") == 0 || strcmp(text, "cpu") == 0) return DLD_ACCELERATION_SOFTWARE;
    if (strcmp(text, "vulkan") == 0) return DLD_ACCELERATION_VULKAN;
    if (strcmp(text, "vaapi") == 0) return DLD_ACCELERATION_VAAPI;
    if (strcmp(text, "amf") == 0) return DLD_ACCELERATION_AMF;
    if (strcmp(text, "cuda") == 0 || strcmp(text, "nvenc") == 0) return DLD_ACCELERATION_CUDA;
    if (strcmp(text, "qsv") == 0) return DLD_ACCELERATION_QSV;
    return DLD_ACCELERATION_SOFTWARE;
}

/*
 * Conversão também usa temporário. A aceleração é uma otimização opcional: se o
 * backend validado falhar para o codec concreto, o mesmo trabalho é repetido em
 * software antes de declarar falha ao usuário.
 */
static bool execute_convert(DldEngine *engine, DldTaskRecord *task, atomic_bool *cancel_flag,
                            DldEngineEventCallback callback, void *userdata, DldAppError *error)
{
    if (task->input_path == NULL || !regular_file(task->input_path)) {
        (void)dld_app_error_set(error, DLD_ERROR_INVALID_MEDIA, "Arquivo de entrada não existe.",
                                "conversão", false, 0);
        return false;
    }
    char *format = dld_json_get_string_copy(task->options_json, "formato", NULL);
    if (format == NULL) format = dld_json_get_string_copy(task->options_json, "format", "mp4");
    char *acceleration_text = dld_json_get_string_copy(task->options_json, "aceleracao", NULL);
    if (acceleration_text == NULL) {
        acceleration_text = dld_json_get_string_copy(task->options_json, "acceleration", "auto");
    }
    if (format == NULL || acceleration_text == NULL) {
        free(format);
        free(acceleration_text);
        return false;
    }

    DldProbeSummary input_probe;
    dld_probe_summary_init(&input_probe);
    if (!probe_file(engine, task->input_path, &input_probe, error)) {
        dld_probe_summary_clear(&input_probe);
        free(format);
        free(acceleration_text);
        return false;
    }
    dld_probe_summary_clear(&input_probe);

    DldAccelerationMode requested = parse_acceleration(acceleration_text);
    DldAccelerationMode resolved = requested;
    bool validated = false;
    if (requested == DLD_ACCELERATION_AUTO) {
        resolved = dld_hardware_resolve_auto(engine->ffmpeg);
        validated = resolved != DLD_ACCELERATION_SOFTWARE;
    } else if (requested != DLD_ACCELERATION_SOFTWARE) {
        DldHardwareProbe probe = {0};
        if (dld_hardware_probe(engine->ffmpeg, requested, &probe, error)) validated = probe.usable;
        dld_hardware_probe_clear(&probe);
        dld_app_error_clear(error);
        if (!validated) resolved = DLD_ACCELERATION_SOFTWARE;
    }

    const char *destination_dir = task->destination != NULL ? task->destination : engine->output_dir;
    if (!ensure_directory(destination_dir, error)) {
        free(format);
        free(acceleration_text);
        return false;
    }
    char *stem = stem_copy(task->input_path);
    if (stem == NULL) {
        free(format);
        free(acceleration_text);
        return false;
    }
    const size_t name_len = strlen(stem) + strlen(format) + 2U;
    char *name = malloc(name_len);
    if (name == NULL) {
        free(stem);
        free(format);
        free(acceleration_text);
        return false;
    }
    (void)snprintf(name, name_len, "%s.%s", stem, format);
    free(stem);
    char *destination = path_join(destination_dir, name);
    free(name);

    char *tmp_root = path_join(engine->data_dir, "tmp");
    if (tmp_root == NULL || !ensure_directory(tmp_root, error)) {
        free(tmp_root);
        free(destination);
        free(format);
        free(acceleration_text);
        return false;
    }
    const size_t tmp_name_len = strlen(task->id) + strlen(format) + 16U;
    char *tmp_name = malloc(tmp_name_len);
    if (tmp_name == NULL) {
        free(tmp_root);
        free(destination);
        free(format);
        free(acceleration_text);
        return false;
    }
    (void)snprintf(tmp_name, tmp_name_len, "%s.part.%s", task->id, format);
    char *temporary = path_join(tmp_root, tmp_name);
    free(tmp_root);
    free(tmp_name);
    if (temporary == NULL || destination == NULL) {
        free(temporary);
        free(destination);
        free(format);
        free(acceleration_text);
        return false;
    }

    DldCommand command;
    DldProcessResult result;
    dld_command_init(&command);
    dld_process_result_init(&result);
    bool ok = dld_build_conversion_command(engine->ffmpeg, task->input_path, temporary, format,
                                           resolved, validated, &command, error) &&
              run_command(&command, 0U, cancel_flag, NULL, NULL, &result, error);

    if (ok && (result.cancelled || cancel_requested(cancel_flag))) {
        (void)dld_app_error_set(error, DLD_ERROR_CANCELLED, "Conversão cancelada.", "conversão", false, 0);
        ok = false;
    } else if (ok && result.exit_code != 0 && validated) {
        /*
         * Um backend pode inicializar corretamente e ainda não aceitar o codec
         * pedido. Nesse caso a garantia do aplicativo é continuar em software,
         * não falhar apenas porque a aceleração opcional ficou indisponível.
         */
        emit(callback, userdata, task, false, 0.0, NULL,
             "Aceleração indisponível para este arquivo; tentando software", NULL);
        dld_command_clear(&command);
        dld_process_result_clear(&result);
        dld_command_init(&command);
        dld_process_result_init(&result);
        dld_app_error_clear(error);
        (void)dld_cleanup_temporary(temporary, NULL);
        ok = dld_build_conversion_command(engine->ffmpeg, task->input_path, temporary, format,
                                          DLD_ACCELERATION_SOFTWARE, false, &command, error) &&
             run_command(&command, 0U, cancel_flag, NULL, NULL, &result, error);
        if (ok && (result.cancelled || cancel_requested(cancel_flag))) {
            (void)dld_app_error_set(error, DLD_ERROR_CANCELLED, "Conversão cancelada.", "conversão", false, 0);
            ok = false;
        }
    }

    if (ok && result.exit_code != 0) {
        (void)dld_app_error_set(error, DLD_ERROR_ENCODER_UNAVAILABLE,
                                result.stderr_text != NULL && *result.stderr_text != '\0' ? result.stderr_text :
                                "FFmpeg falhou ao converter arquivo.",
                                "conversão", true, result.exit_code);
        ok = false;
    }
    dld_command_clear(&command);
    dld_process_result_clear(&result);
    if (!ok) {
        (void)dld_cleanup_temporary(temporary, NULL);
        free(temporary);
        free(destination);
        free(format);
        free(acceleration_text);
        return false;
    }

    DldProbeSummary output_probe;
    dld_probe_summary_init(&output_probe);
    if (!probe_file(engine, temporary, &output_probe, error)) {
        dld_probe_summary_clear(&output_probe);
        (void)dld_cleanup_temporary(temporary, NULL);
        free(temporary);
        free(destination);
        free(format);
        free(acceleration_text);
        return false;
    }
    dld_probe_summary_clear(&output_probe);

    DldPublishedOutput published;
    dld_published_output_init(&published);
    ok = dld_publish_output(temporary, destination, task->collision, &published, error);
    if (ok) {
        (void)set_task_destination(task, published.path);
        emit(callback, userdata, task, true, 100.0, NULL, "Conversão concluída", published.path);
    }
    dld_published_output_clear(&published);
    free(temporary);
    free(destination);
    free(format);
    free(acceleration_text);
    return ok;
}

static bool execute_validate(DldEngine *engine, DldTaskRecord *task,
                             DldEngineEventCallback callback, void *userdata,
                             DldAppError *error)
{
    const char *path = task->input_path != NULL ? task->input_path : task->destination;
    DldProbeSummary summary;
    dld_probe_summary_init(&summary);
    const bool ok = probe_file(engine, path, &summary, error);
    if (ok) emit(callback, userdata, task, true, 100.0, NULL, "Arquivo de mídia válido", path);
    dld_probe_summary_clear(&summary);
    return ok;
}

/*
 * Ponto único para executar uma tarefa persistente. Toda saída passa por aqui,
 * então estados inicial/final e erro são gravados no banco de maneira uniforme
 * para CLI e GTK4.
 */
bool dld_engine_execute_task(DldEngine *engine, DldTaskRecord *task,
                             atomic_bool *cancel_flag,
                             DldEngineEventCallback callback, void *userdata,
                             DldAppError *error)
{
    if (engine == NULL || task == NULL || task->id == NULL) return false;
    task->created_at_ms = task->created_at_ms == 0U ? now_ms() : task->created_at_ms;
    task->updated_at_ms = now_ms();
    task->status = task->kind == DLD_TASK_DOWNLOAD ? DLD_STATUS_DOWNLOADING :
                   task->kind == DLD_TASK_CONVERT ? DLD_STATUS_CONVERTING :
                   task->kind == DLD_TASK_MERGE ? DLD_STATUS_MERGING : DLD_STATUS_VALIDATING;
    task->has_error = false;
    dld_app_error_clear(&task->error);
    /* Persistência é best-effort aqui: falhar ao salvar histórico não impede a mídia de executar. */
    (void)dld_database_put_task(&engine->database, task, error);
    dld_app_error_clear(error);
    emit(callback, userdata, task, false, 0.0, NULL, "Tarefa iniciada", NULL);

    bool ok = false;
    switch (task->kind) {
    case DLD_TASK_DOWNLOAD:
        ok = execute_download(engine, task, cancel_flag, callback, userdata, error);
        break;
    case DLD_TASK_CONVERT:
    case DLD_TASK_MERGE:
        ok = execute_convert(engine, task, cancel_flag, callback, userdata, error);
        break;
    case DLD_TASK_VALIDATE:
        ok = execute_validate(engine, task, callback, userdata, error);
        break;
    }

    if (ok) {
        task->status = DLD_STATUS_COMPLETED;
        task->has_error = false;
        dld_app_error_clear(&task->error);
        emit(callback, userdata, task, true, 100.0, NULL, "Concluído", task->destination);
    } else if ((error != NULL && error->category == DLD_ERROR_CANCELLED) || cancel_requested(cancel_flag)) {
        task->status = DLD_STATUS_CANCELLED;
        task->has_error = error != NULL;
        if (error != NULL) (void)dld_app_error_copy(&task->error, error);
        emit(callback, userdata, task, false, 0.0, NULL, "Cancelado", NULL);
    } else {
        task->status = DLD_STATUS_FAILED;
        task->has_error = error != NULL;
        if (error != NULL) (void)dld_app_error_copy(&task->error, error);
        emit(callback, userdata, task, false, 0.0, NULL,
             error != NULL && error->message != NULL ? error->message : "Falhou", NULL);
    }
    task->updated_at_ms = now_ms();
    DldAppError persist_error;
    dld_app_error_init(&persist_error);
    (void)dld_database_put_task(&engine->database, task, &persist_error);
    dld_app_error_clear(&persist_error);
    return ok;
}
