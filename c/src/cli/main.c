/*
 * Adaptador de linha de comando.
 *
 * A CLI só transforma argumentos do usuário em chamadas da engine. Regras de
 * mídia, persistência e execução permanecem no núcleo para não haver duas
 * implementações diferentes do mesmo comportamento.
 */

#include "downloader/engine.h"

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_help(void)
{
    puts(
        "downloader-cli\n\n"
        "  deps\n"
        "      verifica yt-dlp, ffmpeg e ffprobe\n\n"
        "  analyze <url> [--playlist] [--cookies arquivo | --browser navegador[:perfil]]\n"
        "      analisa metadados sem baixar\n\n"
        "  download <url> [diretorio] [opcoes]\n"
        "      --playlist\n"
        "      --format auto|mp4|mkv|webm|mp3|opus|m4a|flac|wav\n"
        "      --type video+audio|video|audio\n"
        "      --height 480|720|1080|1440|2160\n"
        "      --bitrate auto|128K|192K|256K|320K\n"
        "      --cookies arquivo | --browser navegador[:perfil]\n\n"
        "  convert <arquivo> [formato] [diretorio] [--acceleration modo]\n"
        "      formatos: mp4, mkv, webm, mp3, opus, m4a, flac, wav\n"
        "      modos: auto, software, vulkan, vaapi, amf, cuda, qsv\n"
    );
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

static void print_error(const DldAppError *error)
{
    fprintf(stderr, "erro: %s", error != NULL && error->message != NULL ? error->message : "falha desconhecida");
    if (error != NULL && error->step != NULL) fprintf(stderr, " (%s)", error->step);
    fputc('\n', stderr);
}

static void print_event(const DldEngineEvent *event, void *userdata)
{
    (void)userdata;
    if (event->has_progress) printf("%6.1f%% ", event->progress_percent);
    else printf("         ");
    printf("%-12s", dld_task_status_name(event->status));
    if (event->speed != NULL && *event->speed != '\0') printf(" %-12s", event->speed);
    if (event->message != NULL) printf(" %s", event->message);
    if (event->path != NULL) printf(" — %s", event->path);
    putchar('\n');
    fflush(stdout);
}

static bool parse_auth(int argc, char **argv, int *index, DldAuthRef *auth, bool *has_auth,
                       DldAppError *error)
{
    const char *flag = argv[*index];
    if (strcmp(flag, "--cookies") != 0 && strcmp(flag, "--cookie-file") != 0 &&
        strcmp(flag, "--browser") != 0 && strcmp(flag, "--cookies-from-browser") != 0) return false;
    if (*index + 1 >= argc) {
        (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Valor ausente para autenticação.", "CLI", false, 0);
        return true;
    }
    const DldAuthKind kind = (strcmp(flag, "--cookies") == 0 || strcmp(flag, "--cookie-file") == 0)
                                ? DLD_AUTH_COOKIE_FILE : DLD_AUTH_BROWSER_PROFILE;
    if (!dld_auth_ref_set(auth, kind, argv[++(*index)])) {
        (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Memória insuficiente.", "CLI", false, 0);
    } else {
        *has_auth = true;
    }
    return true;
}

static int command_deps(DldEngine *engine)
{
    DldAppError error;
    dld_app_error_init(&error);
    char *report = NULL;
    const bool ok = dld_engine_check_dependencies(engine, &report, &error);
    if (report != NULL) fputs(report, stdout);
    free(report);
    if (!ok) print_error(&error);
    dld_app_error_clear(&error);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int command_analyze(DldEngine *engine, int argc, char **argv)
{
    if (argc < 3) {
        print_help();
        return EXIT_FAILURE;
    }
    bool playlist = false;
    bool has_auth = false;
    DldAuthRef auth;
    dld_auth_ref_init(&auth);
    DldAppError error;
    dld_app_error_init(&error);
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--playlist") == 0 || strcmp(argv[i], "--lista") == 0) {
            playlist = true;
        } else if (!parse_auth(argc, argv, &i, &auth, &has_auth, &error)) {
            (void)dld_app_error_set(&error, DLD_ERROR_INTERNAL, "Opção de análise desconhecida.", "CLI", false, 0);
            break;
        }
    }
    if (error.message != NULL) {
        print_error(&error);
        dld_auth_ref_clear(&auth);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }
    DldMediaSummary summary;
    dld_media_summary_init(&summary);
    atomic_bool cancelled = false;
    const bool ok = dld_engine_analyze(engine, argv[2], playlist, has_auth ? &auth : NULL,
                                       &cancelled, &summary, &error);
    if (ok) {
        printf("Título: %s\n", summary.title != NULL ? summary.title : "(sem título)");
        if (summary.uploader != NULL) printf("Autor: %s\n", summary.uploader);
        if (summary.id != NULL) printf("ID: %s\n", summary.id);
        if (summary.has_duration) printf("Duração: %.1f s\n", summary.duration_seconds);
        if (summary.is_playlist) printf("Playlist: %zu itens\n", summary.playlist_entries);
    } else print_error(&error);
    dld_media_summary_clear(&summary);
    dld_auth_ref_clear(&auth);
    dld_app_error_clear(&error);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int command_download(DldEngine *engine, int argc, char **argv)
{
    if (argc < 3) {
        print_help();
        return EXIT_FAILURE;
    }
    const char *destination = NULL;
    const char *format = "auto";
    const char *kind = "video+audio";
    const char *bitrate = "auto";
    int max_height = 0;
    bool playlist = false;
    bool has_auth = false;
    DldAuthRef auth;
    dld_auth_ref_init(&auth);
    DldAppError error;
    dld_app_error_init(&error);

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--playlist") == 0 || strcmp(argv[i], "--lista") == 0) playlist = true;
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) format = argv[++i];
        else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) kind = argv[++i];
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) max_height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) bitrate = argv[++i];
        else if (parse_auth(argc, argv, &i, &auth, &has_auth, &error)) { }
        else if (argv[i][0] != '-' && destination == NULL) destination = argv[i];
        else {
            (void)dld_app_error_set(&error, DLD_ERROR_INTERNAL, "Opção de download desconhecida.", "CLI", false, 0);
            break;
        }
    }
    if (error.message != NULL) {
        print_error(&error);
        dld_auth_ref_clear(&auth);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }

    struct json_object *options = json_object_new_object();
    json_object_object_add(options, "playlist", json_object_new_boolean(playlist));
    json_object_object_add(options, "output_format", json_object_new_string(format));
    json_object_object_add(options, "media_kind", json_object_new_string(kind));
    json_object_object_add(options, "bitrate", json_object_new_string(bitrate));
    json_object_object_add(options, "max_height", json_object_new_int(max_height));

    DldTaskRecord task;
    dld_task_record_init(&task);
    task.id = make_task_id("download");
    task.kind = DLD_TASK_DOWNLOAD;
    task.input_url = dld_string_duplicate(argv[2]);
    task.options_json = dld_string_duplicate(json_object_to_json_string_ext(options, JSON_C_TO_STRING_PLAIN));
    task.destination = destination != NULL ? dld_string_duplicate(destination) : NULL;
    task.collision = DLD_COLLISION_RENAME;
    if (has_auth) {
        task.has_auth = dld_auth_ref_copy(&task.auth, &auth);
    }
    json_object_put(options);
    dld_auth_ref_clear(&auth);

    if (task.id == NULL || task.input_url == NULL || task.options_json == NULL) {
        fprintf(stderr, "erro: memória insuficiente\n");
        dld_task_record_clear(&task);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }
    atomic_bool cancelled = false;
    const bool ok = dld_engine_execute_task(engine, &task, &cancelled, print_event, NULL, &error);
    if (!ok) print_error(&error);
    dld_task_record_clear(&task);
    dld_app_error_clear(&error);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int command_convert(DldEngine *engine, int argc, char **argv)
{
    if (argc < 3) {
        print_help();
        return EXIT_FAILURE;
    }
    const char *format = argc >= 4 && argv[3][0] != '-' ? argv[3] : "mp4";
    const char *destination = argc >= 5 && argv[4][0] != '-' ? argv[4] : NULL;
    const char *acceleration = "auto";
    int start = 3;
    if (argc >= 4 && argv[3][0] != '-') start = 4;
    if (argc >= 5 && argv[4][0] != '-') start = 5;
    for (int i = start; i < argc; ++i) {
        if (strcmp(argv[i], "--acceleration") == 0 && i + 1 < argc) acceleration = argv[++i];
        else {
            print_help();
            return EXIT_FAILURE;
        }
    }

    struct json_object *options = json_object_new_object();
    json_object_object_add(options, "formato", json_object_new_string(format));
    json_object_object_add(options, "aceleracao", json_object_new_string(acceleration));
    DldTaskRecord task;
    dld_task_record_init(&task);
    task.id = make_task_id("convert");
    task.kind = DLD_TASK_CONVERT;
    task.input_path = dld_string_duplicate(argv[2]);
    task.options_json = dld_string_duplicate(json_object_to_json_string_ext(options, JSON_C_TO_STRING_PLAIN));
    task.destination = destination != NULL ? dld_string_duplicate(destination) : NULL;
    task.collision = DLD_COLLISION_RENAME;
    json_object_put(options);

    DldAppError error;
    dld_app_error_init(&error);
    atomic_bool cancelled = false;
    const bool ok = dld_engine_execute_task(engine, &task, &cancelled, print_event, NULL, &error);
    if (!ok) print_error(&error);
    dld_task_record_clear(&task);
    dld_app_error_clear(&error);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        print_help();
        return EXIT_SUCCESS;
    }
    DldEngine engine;
    dld_engine_init(&engine);
    DldEngineConfig config = dld_engine_config_default();
    DldAppError error;
    dld_app_error_init(&error);
    if (!dld_engine_open(&engine, &config, &error)) {
        print_error(&error);
        dld_engine_clear(&engine);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    if (strcmp(argv[1], "deps") == 0) result = command_deps(&engine);
    else if (strcmp(argv[1], "analyze") == 0 || strcmp(argv[1], "analisar") == 0) {
        result = command_analyze(&engine, argc, argv);
    }
    else if (strcmp(argv[1], "download") == 0 || strcmp(argv[1], "baixar") == 0) {
        result = command_download(&engine, argc, argv);
    }
    else if (strcmp(argv[1], "convert") == 0 || strcmp(argv[1], "converter") == 0) {
        result = command_convert(&engine, argc, argv);
    }
    else print_help();
    dld_engine_clear(&engine);
    dld_app_error_clear(&error);
    return result;
}
