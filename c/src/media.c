/*
 * Planejamento de comandos de mídia e parsing das respostas estruturadas.
 *
 * Esta camada nunca executa processos. Ela apenas produz `argv` seguro e interpreta
 * JSON/progresso, deixando execução, timeout e publicação para outras camadas.
 */

#include "downloader/media.h"

#include <ctype.h>
#include <errno.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void dld_command_init(DldCommand *command)
{
    if (command == NULL) return;
    command->argv = NULL;
    command->argc = 0U;
}

void dld_command_clear(DldCommand *command)
{
    if (command == NULL) return;
    if (command->argv != NULL) {
        for (size_t i = 0; i < command->argc; ++i) free(command->argv[i]);
    }
    free(command->argv);
    dld_command_init(command);
}

void dld_media_summary_init(DldMediaSummary *summary)
{
    if (summary == NULL) return;
    memset(summary, 0, sizeof(*summary));
}

void dld_media_summary_clear(DldMediaSummary *summary)
{
    if (summary == NULL) return;
    free(summary->id);
    free(summary->title);
    free(summary->uploader);
    dld_media_summary_init(summary);
}

void dld_probe_summary_init(DldProbeSummary *summary)
{
    if (summary == NULL) return;
    memset(summary, 0, sizeof(*summary));
}

void dld_probe_summary_clear(DldProbeSummary *summary)
{
    if (summary == NULL) return;
    free(summary->container);
    dld_probe_summary_init(summary);
}

/*
 * `DldCommand` possui suas próprias cópias dos argumentos. Isso permite montar
 * comandos com strings temporárias sem criar dependência de lifetime no chamador.
 */
static bool command_push(DldCommand *command, const char *argument)
{
    char **grown = realloc(command->argv, (command->argc + 2U) * sizeof(*grown));
    if (grown == NULL) return false;
    command->argv = grown;
    command->argv[command->argc] = dld_string_duplicate(argument);
    if (command->argv[command->argc] == NULL) return false;
    ++command->argc;
    command->argv[command->argc] = NULL;
    return true;
}

static bool command_push_pair(DldCommand *command, const char *a, const char *b)
{
    return command_push(command, a) && command_push(command, b);
}

static bool set_media_error(DldAppError *error, DldErrorCategory category,
                            const char *message, const char *step)
{
    return dld_app_error_set(error, category, message, step, false, 0);
}

bool dld_validate_url(const char *url)
{
    if (url == NULL) return false;
    return strncmp(url, "https://", 8U) == 0 || strncmp(url, "http://", 7U) == 0;
}

bool dld_validate_netscape_cookie_file(const char *path, DldAppError *error)
{
    if (path == NULL || *path == '\0') {
        (void)set_media_error(error, DLD_ERROR_AUTHENTICATION,
                              "Arquivo de cookies não informado.", "autenticação");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        (void)dld_app_error_set(error, DLD_ERROR_AUTHENTICATION,
                                "Arquivo de cookies não existe ou não pode ser lido.",
                                "autenticação", true, errno);
        return false;
    }
    char line[8192];
    bool valid = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *start = line;
        while (isspace((unsigned char)*start)) ++start;
        if (*start == '\0' || *start == '#') continue;
        unsigned tabs = 0U;
        for (const char *p = start; *p != '\0'; ++p) if (*p == '\t') ++tabs;
        if (tabs >= 6U) {
            valid = true;
            break;
        }
    }
    fclose(file);
    if (!valid) {
        (void)set_media_error(error, DLD_ERROR_AUTHENTICATION,
                              "Arquivo de cookies não está no formato Netscape.",
                              "autenticação");
    }
    return valid;
}

bool dld_validate_browser_selector(const char *selector, DldAppError *error)
{
    if (selector == NULL || *selector == '\0') {
        (void)set_media_error(error, DLD_ERROR_AUTHENTICATION,
                              "Perfil de navegador vazio.", "autenticação");
        return false;
    }
    const char *end = strpbrk(selector, ":+");
    const size_t length = end == NULL ? strlen(selector) : (size_t)(end - selector);
    static const char *supported[] = {
        "brave", "chrome", "chromium", "edge", "firefox", "opera", "safari", "vivaldi"
    };
    for (size_t i = 0U; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        if (strlen(supported[i]) == length && strncasecmp(selector, supported[i], length) == 0) {
            return true;
        }
    }
    (void)set_media_error(error, DLD_ERROR_AUTHENTICATION,
                          "Navegador não suportado pelo modo de cookies.", "autenticação");
    return false;
}

static bool append_auth(DldCommand *command, const DldAuthRef *auth, DldAppError *error)
{
    if (auth == NULL || auth->kind == DLD_AUTH_NONE) return true;
    if (auth->kind == DLD_AUTH_COOKIE_FILE) {
        if (!dld_validate_netscape_cookie_file(auth->id, error)) return false;
        return command_push_pair(command, "--cookies", auth->id);
    }
    if (auth->kind == DLD_AUTH_BROWSER_PROFILE) {
        if (!dld_validate_browser_selector(auth->id, error)) return false;
        return command_push_pair(command, "--cookies-from-browser", auth->id);
    }
    return false;
}

static bool begin_command(DldCommand *command, const char *program, DldAppError *error)
{
    dld_command_clear(command);
    if (program == NULL || *program == '\0' || !command_push(command, program)) {
        (void)set_media_error(error, DLD_ERROR_INTERNAL,
                              "Memória insuficiente para criar comando.", "planejamento");
        return false;
    }
    return true;
}

bool dld_build_analysis_command(const char *yt_dlp, const char *url, bool playlist,
                                const DldAuthRef *auth, DldCommand *command,
                                DldAppError *error)
{
    if (!dld_validate_url(url)) {
        (void)set_media_error(error, DLD_ERROR_INVALID_URL, "URL inválida.", "análise");
        return false;
    }
    if (!begin_command(command, yt_dlp, error)) return false;
    if (!command_push(command, "-J") || !command_push(command, "--simulate")) goto oom;
    if (playlist) {
        if (!command_push(command, "--flat-playlist")) goto oom;
    } else if (!command_push(command, "--no-playlist")) goto oom;
    if (!append_auth(command, auth, error)) return false;
    if (!command_push(command, url)) goto oom;
    return true;
oom:
    (void)set_media_error(error, DLD_ERROR_INTERNAL, "Memória insuficiente para criar comando.", "planejamento");
    return false;
}

static char *build_format_selector(const char *media_kind, unsigned max_height)
{
    const char *kind = media_kind == NULL ? "video+audio" : media_kind;
    char height[64] = "";
    if (max_height > 0U) (void)snprintf(height, sizeof(height), "[height<=%u]", max_height);
    char selector[256];
    if (strcmp(kind, "audio") == 0 || strcmp(kind, "audio-only") == 0) {
        (void)snprintf(selector, sizeof(selector), "bestaudio/best");
    } else if (strcmp(kind, "video") == 0 || strcmp(kind, "video-only") == 0) {
        (void)snprintf(selector, sizeof(selector), "bestvideo%s", height);
    } else {
        (void)snprintf(selector, sizeof(selector), "bestvideo%s+bestaudio/best%s", height, height);
    }
    return dld_string_duplicate(selector);
}

static bool is_audio_format(const char *format)
{
    return format != NULL && (strcmp(format, "mp3") == 0 || strcmp(format, "opus") == 0 ||
                              strcmp(format, "m4a") == 0 || strcmp(format, "flac") == 0 ||
                              strcmp(format, "wav") == 0);
}

/*
 * Traduz opções de interface em argumentos do yt-dlp. A ordem é intencional:
 * opções e autenticação vêm antes da URL, e cada valor ocupa seu próprio argv.
 */
bool dld_build_download_command(const char *yt_dlp, const char *url,
                                const char *output_template, bool playlist,
                                const char *media_kind, const char *format,
                                unsigned max_height, const char *bitrate,
                                bool youtube_protection, const DldAuthRef *auth,
                                DldCommand *command, DldAppError *error)
{
    if (!dld_validate_url(url)) {
        (void)set_media_error(error, DLD_ERROR_INVALID_URL, "URL inválida.", "download");
        return false;
    }
    if (output_template == NULL || *output_template == '\0') {
        (void)set_media_error(error, DLD_ERROR_INTERNAL, "Destino de download vazio.", "download");
        return false;
    }
    if (!begin_command(command, yt_dlp, error)) return false;
    if (!playlist && !command_push(command, "--no-playlist")) goto oom;
    if (!command_push(command, "--newline") ||
        !command_push_pair(command, "--progress-template", "percent=%(progress._percent_str)s eta=%(progress.eta)s speed=%(progress._speed_str)s")) goto oom;
    if (youtube_protection) {
        if (!command_push_pair(command, "--sleep-interval", "5") ||
            !command_push_pair(command, "--retries", "3") ||
            !command_push_pair(command, "--fragment-retries", "3")) goto oom;
    }

    char *selector = build_format_selector(media_kind, max_height);
    if (selector == NULL) goto oom;
    const bool selector_ok = command_push_pair(command, "-f", selector);
    free(selector);
    if (!selector_ok) goto oom;

    if (format != NULL && strcmp(format, "auto") != 0) {
        if (is_audio_format(format)) {
            if (!command_push(command, "-x") || !command_push_pair(command, "--audio-format", format)) goto oom;
            if (bitrate != NULL && strcmp(bitrate, "auto") != 0 &&
                !command_push_pair(command, "--audio-quality", bitrate)) goto oom;
            if ((strcmp(format, "mp3") == 0 || strcmp(format, "opus") == 0) &&
                (!command_push(command, "--embed-thumbnail") || !command_push(command, "--add-metadata"))) goto oom;
        } else {
            if (!command_push_pair(command, "--merge-output-format", format)) goto oom;
            if ((strcmp(format, "mp4") == 0 || strcmp(format, "mkv") == 0) &&
                (!command_push(command, "--embed-thumbnail") || !command_push(command, "--add-metadata"))) goto oom;
        }
    }
    if (!append_auth(command, auth, error)) return false;
    if (!command_push_pair(command, "-o", output_template) || !command_push(command, url)) goto oom;
    return true;
oom:
    (void)set_media_error(error, DLD_ERROR_INTERNAL, "Memória insuficiente para criar comando.", "planejamento");
    return false;
}

bool dld_build_ffprobe_command(const char *ffprobe, const char *input,
                               DldCommand *command, DldAppError *error)
{
    if (input == NULL || *input == '\0') return false;
    if (!begin_command(command, ffprobe, error)) return false;
    const char *args[] = {"-v", "error", "-show_streams", "-show_format", "-of", "json", input};
    for (size_t i = 0U; i < sizeof(args) / sizeof(args[0]); ++i) {
        if (!command_push(command, args[i])) return false;
    }
    return true;
}

static const char *acceleration_name(DldAccelerationMode mode)
{
    switch (mode) {
    case DLD_ACCELERATION_VULKAN: return "vulkan";
    case DLD_ACCELERATION_VAAPI: return "vaapi";
    case DLD_ACCELERATION_AMF: return "amf";
    case DLD_ACCELERATION_CUDA: return "cuda";
    case DLD_ACCELERATION_QSV: return "qsv";
    default: return NULL;
    }
}

static const char *accelerated_encoder(DldAccelerationMode mode, const char *software)
{
    if (strcmp(software, "libx264") == 0) {
        switch (mode) {
        case DLD_ACCELERATION_VULKAN: return "h264_vulkan";
        case DLD_ACCELERATION_VAAPI: return "h264_vaapi";
        case DLD_ACCELERATION_AMF: return "h264_amf";
        case DLD_ACCELERATION_CUDA: return "h264_nvenc";
        case DLD_ACCELERATION_QSV: return "h264_qsv";
        default: break;
        }
    }
    if (strcmp(software, "libx265") == 0) {
        switch (mode) {
        case DLD_ACCELERATION_VULKAN: return "hevc_vulkan";
        case DLD_ACCELERATION_VAAPI: return "hevc_vaapi";
        case DLD_ACCELERATION_AMF: return "hevc_amf";
        case DLD_ACCELERATION_CUDA: return "hevc_nvenc";
        case DLD_ACCELERATION_QSV: return "hevc_qsv";
        default: break;
        }
    }
    return software;
}

bool dld_build_conversion_command(const char *ffmpeg, const char *input,
                                  const char *output, const char *format,
                                  DldAccelerationMode acceleration,
                                  bool acceleration_validated,
                                  DldCommand *command, DldAppError *error)
{
    if (input == NULL || output == NULL) return false;
    const char *fmt = format == NULL ? "mp4" : format;
    if (!begin_command(command, ffmpeg, error)) return false;
    if (!command_push(command, "-hide_banner") || !command_push(command, "-nostdin")) goto oom;
    const char *hw = acceleration_validated ? acceleration_name(acceleration) : NULL;
    if (hw != NULL && !command_push_pair(command, "-hwaccel", hw)) goto oom;
    if (!command_push_pair(command, "-i", input)) goto oom;

    if (strcmp(fmt, "mp3") == 0) {
        if (!command_push(command, "-vn") || !command_push_pair(command, "-c:a", "libmp3lame")) goto oom;
    } else if (strcmp(fmt, "opus") == 0) {
        if (!command_push(command, "-vn") || !command_push_pair(command, "-c:a", "libopus")) goto oom;
    } else if (strcmp(fmt, "m4a") == 0) {
        if (!command_push(command, "-vn") || !command_push_pair(command, "-c:a", "aac")) goto oom;
    } else if (strcmp(fmt, "flac") == 0) {
        if (!command_push(command, "-vn") || !command_push_pair(command, "-c:a", "flac")) goto oom;
    } else if (strcmp(fmt, "wav") == 0) {
        if (!command_push(command, "-vn") || !command_push_pair(command, "-c:a", "pcm_s16le")) goto oom;
    } else if (strcmp(fmt, "mkv") == 0) {
        if (!command_push_pair(command, "-c", "copy")) goto oom;
    } else if (strcmp(fmt, "webm") == 0) {
        if (!command_push_pair(command, "-c:v", "libvpx-vp9") ||
            !command_push_pair(command, "-c:a", "libopus")) goto oom;
    } else {
        const char *video = hw == NULL ? "libx264" : accelerated_encoder(acceleration, "libx264");
        if (!command_push_pair(command, "-c:v", video) || !command_push_pair(command, "-c:a", "aac")) goto oom;
    }
    if (!command_push(command, "-y") || !command_push(command, output)) goto oom;
    return true;
oom:
    (void)set_media_error(error, DLD_ERROR_INTERNAL, "Memória insuficiente para criar comando.", "planejamento");
    return false;
}

static char *json_string_copy(struct json_object *object, const char *key)
{
    struct json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || value == NULL ||
        !json_object_is_type(value, json_type_string)) return NULL;
    return dld_string_duplicate(json_object_get_string(value));
}

/*
 * Para playlists, a UI precisa de contagem + uma prévia representativa. Por isso
 * o resumo marca a resposta como playlist e usa o primeiro item para título/id.
 */
bool dld_parse_ytdlp_summary(const char *json_text, DldMediaSummary *summary,
                             DldAppError *error)
{
    if (json_text == NULL || summary == NULL) return false;
    struct json_tokener *tokener = json_tokener_new();
    if (tokener == NULL) return false;
    struct json_object *root = json_tokener_parse_ex(tokener, json_text, (int)strlen(json_text));
    const enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    if (root == NULL || parse_error != json_tokener_success) {
        if (root != NULL) json_object_put(root);
        (void)set_media_error(error, DLD_ERROR_INVALID_MEDIA, "JSON do yt-dlp inválido.", "análise");
        return false;
    }

    dld_media_summary_clear(summary);
    struct json_object *object = root;
    struct json_object *entries = NULL;
    if (json_object_object_get_ex(root, "entries", &entries) &&
        json_object_is_type(entries, json_type_array)) {
        summary->is_playlist = true;
        summary->playlist_entries = json_object_array_length(entries);
        if (summary->playlist_entries > 0U) object = json_object_array_get_idx(entries, 0U);
    }
    if (object != NULL && json_object_is_type(object, json_type_object)) {
        summary->id = json_string_copy(object, "id");
        summary->title = json_string_copy(object, "title");
        summary->uploader = json_string_copy(object, "uploader");
        struct json_object *duration = NULL;
        if (json_object_object_get_ex(object, "duration", &duration) && duration != NULL &&
            (json_object_is_type(duration, json_type_double) || json_object_is_type(duration, json_type_int))) {
            summary->duration_seconds = json_object_get_double(duration);
            summary->has_duration = summary->duration_seconds >= 0.0;
        }
    }
    json_object_put(root);
    return true;
}

bool dld_parse_ffprobe_summary(const char *json_text, DldProbeSummary *summary,
                               DldAppError *error)
{
    if (json_text == NULL || summary == NULL) return false;
    struct json_object *root = json_tokener_parse(json_text);
    if (root == NULL) {
        (void)set_media_error(error, DLD_ERROR_INVALID_MEDIA, "JSON do ffprobe inválido.", "validação");
        return false;
    }
    dld_probe_summary_clear(summary);
    struct json_object *streams = NULL;
    if (json_object_object_get_ex(root, "streams", &streams) && json_object_is_type(streams, json_type_array)) {
        const size_t count = json_object_array_length(streams);
        for (size_t i = 0U; i < count; ++i) {
            struct json_object *stream = json_object_array_get_idx(streams, i);
            struct json_object *type = NULL;
            if (stream != NULL && json_object_object_get_ex(stream, "codec_type", &type) && type != NULL) {
                const char *name = json_object_get_string(type);
                if (strcmp(name, "video") == 0) ++summary->video_streams;
                if (strcmp(name, "audio") == 0) ++summary->audio_streams;
            }
        }
    }
    struct json_object *format = NULL;
    if (json_object_object_get_ex(root, "format", &format) && json_object_is_type(format, json_type_object)) {
        summary->container = json_string_copy(format, "format_name");
        struct json_object *duration = NULL;
        if (json_object_object_get_ex(format, "duration", &duration) && duration != NULL) {
            const char *text = json_object_get_string(duration);
            char *end = NULL;
            const double seconds = strtod(text, &end);
            if (end != text && isfinite(seconds) && seconds >= 0.0) {
                summary->duration_seconds = seconds;
                summary->has_duration = true;
            }
        }
    }
    summary->valid = summary->video_streams > 0U || summary->audio_streams > 0U;
    json_object_put(root);
    if (!summary->valid) {
        (void)set_media_error(error, DLD_ERROR_INVALID_MEDIA, "Nenhum stream de mídia encontrado.", "validação");
    }
    return summary->valid;
}

DldProgress dld_parse_progress_line(const char *line)
{
    DldProgress progress;
    memset(&progress, 0, sizeof(progress));
    if (line == NULL) return progress;
    char *copy = dld_string_duplicate(line);
    if (copy == NULL) return progress;
    char *save = NULL;
    for (char *part = strtok_r(copy, " \t", &save); part != NULL; part = strtok_r(NULL, " \t", &save)) {
        char *equals = strchr(part, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        const char *value = equals + 1;
        if (strcmp(part, "percent") == 0) {
            char *end = NULL;
            const double parsed = strtod(value, &end);
            if (end != value) {
                progress.has_percent = true;
                progress.percent = parsed;
            }
        } else if (strcmp(part, "eta") == 0 && strcmp(value, "NA") != 0) {
            char *end = NULL;
            const double parsed = strtod(value, &end);
            if (end != value) {
                progress.has_eta = true;
                progress.eta_seconds = parsed;
            }
        } else if (strcmp(part, "speed") == 0) {
            (void)snprintf(progress.speed, sizeof(progress.speed), "%s", value);
        }
    }
    free(copy);
    return progress;
}

/* Centraliza a validação do JSON de opções para os getters simples abaixo. */
static struct json_object *parse_options(const char *json_text)
{
    if (json_text == NULL || *json_text == '\0') return json_tokener_parse("{}");
    struct json_object *root = json_tokener_parse(json_text);
    if (root == NULL || !json_object_is_type(root, json_type_object)) {
        if (root != NULL) json_object_put(root);
        return NULL;
    }
    return root;
}

char *dld_json_get_string_copy(const char *json_text, const char *key, const char *fallback)
{
    struct json_object *root = parse_options(json_text);
    if (root == NULL) return dld_string_duplicate(fallback);
    struct json_object *value = NULL;
    char *result = NULL;
    if (json_object_object_get_ex(root, key, &value) && value != NULL &&
        json_object_is_type(value, json_type_string)) {
        result = dld_string_duplicate(json_object_get_string(value));
    } else {
        result = dld_string_duplicate(fallback);
    }
    json_object_put(root);
    return result;
}

bool dld_json_get_bool(const char *json_text, const char *key, bool fallback)
{
    struct json_object *root = parse_options(json_text);
    if (root == NULL) return fallback;
    struct json_object *value = NULL;
    bool result = fallback;
    if (json_object_object_get_ex(root, key, &value) && value != NULL) {
        result = json_object_get_boolean(value) != 0;
    }
    json_object_put(root);
    return result;
}

int dld_json_get_int(const char *json_text, const char *key, int fallback)
{
    struct json_object *root = parse_options(json_text);
    if (root == NULL) return fallback;
    struct json_object *value = NULL;
    int result = fallback;
    if (json_object_object_get_ex(root, key, &value) && value != NULL &&
        json_object_is_type(value, json_type_int)) result = json_object_get_int(value);
    json_object_put(root);
    return result;
}
