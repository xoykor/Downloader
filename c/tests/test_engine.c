#include "downloader/engine.h"
#include "downloader/process.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    size_t child_progress_events;
    size_t child_completed_events;
    char child_completed_path[768];
    char parent_terminal_message[1024];
} EventStats;

static void write_le16(FILE *file, uint16_t value)
{
    fputc((int)(value & 0xffU), file);
    fputc((int)((value >> 8U) & 0xffU), file);
}

static void write_le32(FILE *file, uint32_t value)
{
    for (unsigned shift = 0U; shift < 4U; ++shift) {
        fputc((int)((value >> (8U * shift)) & 0xffU), file);
    }
}

static void write_wav(const char *path)
{
    const uint32_t samples = 800U;
    const uint32_t data_size = samples * 2U;

    FILE *file = fopen(path, "wb");
    assert(file != NULL);

    fwrite("RIFF", 1U, 4U, file);
    write_le32(file, 36U + data_size);
    fwrite("WAVE", 1U, 4U, file);

    fwrite("fmt ", 1U, 4U, file);
    write_le32(file, 16U);
    write_le16(file, 1U);
    write_le16(file, 1U);
    write_le32(file, 8000U);
    write_le32(file, 16000U);
    write_le16(file, 2U);
    write_le16(file, 16U);

    fwrite("data", 1U, 4U, file);
    write_le32(file, data_size);
    for (uint32_t sample = 0U; sample < samples; ++sample) {
        write_le16(file, 0U);
    }

    assert(fclose(file) == 0);
}

/*
 * Simula o comportamento relevante do yt-dlp:
 * - a análise anuncia uma playlist com duas faixas;
 * - o download produz a primeira faixa e seus eventos de progresso;
 * - a primeira execução falha na segunda faixa;
 * - a retomada seleciona apenas o índice 2 e a conclui.
 */
static void write_fake_ytdlp(const char *path, const char *fixture)
{
    FILE *script = fopen(path, "wb");
    assert(script != NULL);

    fputs("#!/bin/sh\n", script);
    fputs("analysis=0\n", script);
    fputs("for arg in \"$@\"; do\n", script);
    fputs("  if [ \"$arg\" = \"-J\" ]; then analysis=1; fi\n", script);
    fputs("done\n", script);

    fputs("if [ \"$analysis\" = \"1\" ]; then\n", script);
    fputs(
        "  printf '%s\\n' "
        "'{\"entries\":["
        "{\"id\":\"id1\",\"title\":\"Faixa Um\"},"
        "{\"id\":\"id2\",\"title\":\"Faixa Dois\"}"
        "]}'\n",
        script);
    fputs("  exit 0\n", script);
    fputs("fi\n", script);

    fputs("output=''\n", script);
    fputs("previous=''\n", script);
    fputs("items=''\n", script);
    fputs("for arg in \"$@\"; do\n", script);
    fputs("  if [ \"$previous\" = \"-o\" ]; then output=\"$arg\"; break; fi\n", script);
    fputs("  if [ \"$previous\" = \"--playlist-items\" ]; then items=\"$arg\"; fi\n", script);
    fputs("  previous=\"$arg\"\n", script);
    fputs("done\n", script);
    fputs("dir=${output%/*}\n", script);

    fprintf(script, "if [ \"$items\" = \"2\" ]; then\n  cp '%s' \"$dir/Faixa Dois [id2].wav\"\n", fixture);
    fputs("  printf '%s\\n' 'POLICY_VIDEO id2'\n", script);
    fputs("  printf '%s\\n' 'TRACK {\"id\":\"id2\",\"title\":\"Faixa Dois\",\"playlist_index\":2,\"playlist_count\":2} {\"_percent_str\":\"100.0%%\",\"_speed_str\":\"1.0MiB/s\",\"eta\":0,\"status\":\"finished\"}' >&2\n", script);
    fputs("  printf 'FILE {\"id\":\"id2\",\"title\":\"Faixa Dois\",\"playlist_index\":2,\"playlist_count\":2,\"filepath\":\"%s/Faixa Dois [id2].wav\"}\\n' \"$dir\"\n", script);
    fputs("  exit 0\nfi\n", script);

    fprintf(
        script,
        "cp '%s' \"$dir/Faixa Um [id1].wav\"\n",
        fixture);

    fputs("printf '%s\\n' 'POLICY_VIDEO id1'\n", script);

    fputs(
        "printf '%s\\n' "
        "'TRACK {\"id\":\"id1\",\"title\":\"Faixa Um\","
        "\"playlist_index\":1,\"playlist_count\":2} "
        "{\"_percent_str\":\"42.0%%\",\"_speed_str\":\"1.0MiB/s\","
        "\"eta\":1,\"status\":\"downloading\"}' >&2\n",
        script);

    fputs(
        "printf 'FILE "
        "{\"id\":\"id1\",\"title\":\"Faixa Um\","
        "\"playlist_index\":1,\"playlist_count\":2,"
        "\"filepath\":\"%s/Faixa Um [id1].wav\"}\\n' "
        "\"$dir\"\n",
        script);

    /*
     * O processo continua vivo depois do FILE. O arquivo precisa desaparecer do
     * staging enquanto a playlist ainda está em execução; caso contrário o teste
     * remove a origem para impedir a publicação tardia após o exit.
     */
    fputs("i=0\n", script);
    fputs("while [ -e \"$dir/Faixa Um [id1].wav\" ] && [ \"$i\" -lt 40 ]; do\n", script);
    fputs("  sleep 0.05\n", script);
    fputs("  i=$((i + 1))\n", script);
    fputs("done\n", script);
    fputs("if [ -e \"$dir/Faixa Um [id1].wav\" ]; then\n", script);
    fputs("  rm -f \"$dir/Faixa Um [id1].wav\"\n", script);
    fputs("  printf '%s\\n' 'faixa não foi publicada durante a playlist' >&2\n", script);
    fputs("  exit 0\n", script);
    fputs("fi\n", script);

    fputs("printf '%s\\n' 'ERROR: [youtube] id2: Video unavailable' >&2\n", script);
    fputs("exit 1\n", script);

    assert(fclose(script) == 0);
    assert(chmod(path, 0755) == 0);
}

/*
 * Simula o yt-dlp com DUAS faixas válidas: ambos os arquivos são copiados para
 * o staging e ambos os eventos FILE/TRACK são emitidos, terminando com código 0.
 * Usado para verificar que um re-download parcial de playlist não gera cópias
 * "(1)" dos itens já existentes (política de colisão padrão = renomeio).
 */
static void write_both_fake_ytdlp(const char *path, const char *fixture)
{
    FILE *script = fopen(path, "wb");
    assert(script != NULL);

    fputs("#!/bin/sh\n", script);
    fputs("analysis=0\n", script);
    fputs("for arg in \"$@\"; do\n", script);
    fputs("  if [ \"$arg\" = \"-J\" ]; then analysis=1; fi\n", script);
    fputs("done\n", script);

    fputs("if [ \"$analysis\" = \"1\" ]; then\n", script);
    fputs(
        "  printf '%s\\n' "
        "'{\"entries\":["
        "{\"id\":\"id1\",\"title\":\"Faixa Um\"},"
        "{\"id\":\"id2\",\"title\":\"Faixa Dois\"}"
        "]}'\n",
        script);
    fputs("  exit 0\n", script);
    fputs("fi\n", script);

    fputs("output=''\n", script);
    fputs("previous=''\n", script);
    fputs("for arg in \"$@\"; do\n", script);
    fputs("  if [ \"$previous\" = \"-o\" ]; then output=\"$arg\"; break; fi\n", script);
    fputs("  previous=\"$arg\"\n", script);
    fputs("done\n", script);
    fputs("dir=${output%/*}\n", script);

    fprintf(script, "cp '%s' \"$dir/Faixa Um [id1].wav\"\n", fixture);
    fprintf(script, "cp '%s' \"$dir/Faixa Dois [id2].wav\"\n", fixture);

    fputs(
        "printf '%s\\n' 'POLICY_VIDEO id1'\n"
        "printf '%s\\n' "
        "'TRACK {\"id\":\"id1\",\"title\":\"Faixa Um\","
        "\"playlist_index\":1,\"playlist_count\":2} "
        "{\"_percent_str\":\"50.0%%\",\"_speed_str\":\"1.0MiB/s\","
        "\"eta\":1,\"status\":\"downloading\"}' >&2\n",
        script);
    fputs(
        "printf 'FILE "
        "{\"id\":\"id1\",\"title\":\"Faixa Um\","
        "\"playlist_index\":1,\"playlist_count\":2,"
        "\"filepath\":\"%s/Faixa Um [id1].wav\"}\\n' "
        "\"$dir\"\n",
        script);

    fputs(
        "printf '%s\\n' 'POLICY_VIDEO id2'\n"
        "printf '%s\\n' "
        "'TRACK {\"id\":\"id2\",\"title\":\"Faixa Dois\","
        "\"playlist_index\":2,\"playlist_count\":2} "
        "{\"_percent_str\":\"75.0%%\",\"_speed_str\":\"1.0MiB/s\","
        "\"eta\":1,\"status\":\"downloading\"}' >&2\n",
        script);
    fputs(
        "printf 'FILE "
        "{\"id\":\"id2\",\"title\":\"Faixa Dois\","
        "\"playlist_index\":2,\"playlist_count\":2,"
        "\"filepath\":\"%s/Faixa Dois [id2].wav\"}\\n' "
        "\"$dir\"\n",
        script);

    fputs("printf '%s\\n' 'item id2 concluída' >&2\n", script);
    fputs("exit 0\n", script);

    assert(fclose(script) == 0);
    assert(chmod(path, 0755) == 0);
}

static void capture_event(const DldEngineEvent *event, void *userdata)
{
    EventStats *stats = userdata;
    if (event == NULL || event->task_id == NULL) return;
    if (strstr(event->task_id, "::") == NULL) {
        if (event->status == DLD_STATUS_PARTIAL && event->message != NULL) {
            (void)snprintf(stats->parent_terminal_message,
                           sizeof(stats->parent_terminal_message), "%s", event->message);
        }
        return;
    }

    if (event->has_progress) {
        ++stats->child_progress_events;
    }
    if (event->status == DLD_STATUS_COMPLETED) {
        ++stats->child_completed_events;
        if (event->path != NULL) {
            (void)snprintf(
                stats->child_completed_path,
                sizeof(stats->child_completed_path),
                "%s",
                event->path);
        }
    }
}

static void test_conversion(DldEngine *engine,
                            const char *input,
                            DldAppError *error)
{
    DldTaskRecord task;
    dld_task_record_init(&task);

    task.id = dld_string_duplicate("convert-test");
    task.kind = DLD_TASK_CONVERT;
    task.input_path = dld_string_duplicate(input);
    task.options_json = dld_string_duplicate(
        "{\"formato\":\"mp3\",\"aceleracao\":\"software\"}");
    task.collision = DLD_COLLISION_RENAME;

    atomic_bool cancel = false;
    assert(dld_engine_execute_task(
        engine,
        &task,
        &cancel,
        NULL,
        NULL,
        error));
    assert(task.status == DLD_STATUS_COMPLETED);
    assert(task.destination != NULL);
    assert(access(task.destination, F_OK) == 0);

    assert(unlink(task.destination) == 0);
    dld_task_record_clear(&task);
}

static void test_resume_playlist_after_failure(DldEngine *engine,
                                  const char *output,
                                  DldAppError *error)
{
    DldTaskRecord task;
    dld_task_record_init(&task);

    task.id = dld_string_duplicate("playlist-test");
    task.kind = DLD_TASK_DOWNLOAD;
    task.input_url = dld_string_duplicate(
        "https://www.youtube.com/playlist?list=test");
    task.options_json = dld_string_duplicate(
        "{\"playlist\":true,\"media_kind\":\"video+audio\","
        "\"output_format\":\"auto\",\"bitrate\":\"auto\","
        "\"max_height\":0}");
    task.collision = DLD_COLLISION_RENAME;

    EventStats stats = {0};
    atomic_bool cancel = false;

    assert(dld_engine_execute_task(
        engine,
        &task,
        &cancel,
        capture_event,
        &stats,
        error));

    assert(task.status == DLD_STATUS_COMPLETED);
    assert(task.error.message == NULL);
    assert(stats.child_progress_events > 0U);
    assert(stats.child_completed_events > 0U);
    assert(stats.child_completed_path[0] != '\0');
    assert(strncmp(
        stats.child_completed_path,
        output,
        strlen(output)) == 0);

    unsigned youtube_starts = 0U;
    uint64_t oldest_start = 0U;
    assert(dld_database_count_youtube_starts_since(
        &engine->database,
        0U,
        &youtube_starts,
        &oldest_start,
        error));
    assert(youtube_starts == 2U);
    assert(oldest_start > 0U);

    char published[768];
    (void)snprintf(
        published,
        sizeof(published),
        "%s/Faixa Um [id1].wav",
        output);
    assert(access(published, F_OK) == 0);

    char resumed[768];
    (void)snprintf(resumed, sizeof(resumed), "%s/Faixa Dois [id2].wav", output);
    assert(access(resumed, F_OK) == 0);
    assert(unlink(resumed) == 0);

    assert(unlink(published) == 0);

    char library[768];
    (void)snprintf(
        library,
        sizeof(library),
        "%s/.downloader-library.json",
        output);
    (void)unlink(library);

    dld_task_record_clear(&task);
}

/*
 * Re-download parcial de playlist: após o primeiro download as duas faixas
 * existem; apaga-se uma do disco (mantendo-a no índice) e re-executa a mesma
 * playlist. A faixa existente deve ser pulada por entrada — sem gerar cópia
 * "(1)" com a política de renomeio padrão — e apenas a apagada é restaurada.
 */
static void test_playlist_redownload_no_duplicates(
    DldEngine *engine, const char *output, DldAppError *error)
{
    static const char *options =
        "{\"playlist\":true,\"media_kind\":\"video+audio\","
        "\"output_format\":\"auto\",\"bitrate\":\"auto\",\"max_height\":0}";

    /* Primeiro download: ambas as faixas são válidas e ficam no destino. */
    DldTaskRecord task;
    dld_task_record_init(&task);
    task.id = dld_string_duplicate("playlist-redownload");
    task.kind = DLD_TASK_DOWNLOAD;
    task.input_url = dld_string_duplicate(
        "https://www.youtube.com/playlist?list=test");
    task.options_json = dld_string_duplicate(options);
    task.collision = DLD_COLLISION_RENAME;

    EventStats stats = {0};
    atomic_bool cancel = false;
    assert(dld_engine_execute_task(engine, &task, &cancel, capture_event, &stats, error));
    dld_app_error_clear(error);
    dld_task_record_clear(&task);

    char f1[768];
    char f2[768];
    (void)snprintf(f1, sizeof(f1), "%s/Faixa Um [id1].wav", output);
    (void)snprintf(f2, sizeof(f2), "%s/Faixa Dois [id2].wav", output);
    assert(access(f1, F_OK) == 0);
    assert(access(f2, F_OK) == 0);

    /* Apaga uma faixa do disco mas mantém o registro no índice. No re-download
     * ela deve ser restaurada pelo nome original — sem cópia "(1)". */
    assert(unlink(f2) == 0);

    dld_task_record_init(&task);
    task.id = dld_string_duplicate("playlist-redownload");
    task.kind = DLD_TASK_DOWNLOAD;
    task.input_url = dld_string_duplicate(
        "https://www.youtube.com/playlist?list=test");
    task.options_json = dld_string_duplicate(options);
    task.collision = DLD_COLLISION_RENAME;

    EventStats stats2 = {0};
    atomic_bool cancel2 = false;
    assert(dld_engine_execute_task(engine, &task, &cancel2, capture_event, &stats2, error));
    dld_app_error_clear(error);
    dld_task_record_clear(&task);

    /* Ambas as faixas devem estar presentes novamente. */
    assert(access(f1, F_OK) == 0);
    assert(access(f2, F_OK) == 0);

    /* Nenhum arquivo de mídia pode ser uma cópia "(1)". */
    DIR *dir = opendir(output);
    assert(dir != NULL);
    struct dirent *entry;
    int duplicates = 0;
    fputs("DEBUG listing: ", stderr);
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, " (1)") != NULL) ++duplicates;
        fputc('\n', stderr);
        fputs(entry->d_name, stderr);
    }
    closedir(dir);
    assert(duplicates == 0);
}

static void cleanup_test_tree(const char *root,
                              const char *data,
                              const char *output,
                              const char *input,
                              const char *fake_ytdlp)
{
    (void)unlink(input);
    (void)unlink(fake_ytdlp);

    char database[640];
    char wal[656];
    char shm[656];
    char temporary_root[640];

    (void)snprintf(database, sizeof(database), "%s/tasks.sqlite", data);
    (void)snprintf(wal, sizeof(wal), "%s-wal", database);
    (void)snprintf(shm, sizeof(shm), "%s-shm", database);
    (void)snprintf(temporary_root, sizeof(temporary_root), "%s/tmp", data);

    (void)unlink(database);
    (void)unlink(wal);
    (void)unlink(shm);
    (void)rmdir(temporary_root);
    (void)rmdir(output);
    (void)rmdir(data);
    (void)rmdir(root);
}

int main(void)
{
    char *ffmpeg_path = NULL;
    char *ffprobe_path = NULL;

    if (!dld_process_detect("ffmpeg", &ffmpeg_path, NULL) ||
        !dld_process_detect("ffprobe", &ffprobe_path, NULL)) {
        free(ffmpeg_path);
        free(ffprobe_path);
        puts("test_engine: skipped (ffmpeg/ffprobe ausentes)");
        return 0;
    }

    char root[] = "/tmp/downloader-engine-XXXXXX";
    assert(mkdtemp(root) != NULL);

    char data[512];
    char output[512];
    char input[512];
    char fake_ytdlp[512];

    (void)snprintf(data, sizeof(data), "%s/data", root);
    (void)snprintf(output, sizeof(output), "%s/out", root);
    (void)snprintf(input, sizeof(input), "%s/input.wav", root);
    (void)snprintf(fake_ytdlp, sizeof(fake_ytdlp), "%s/fake-yt-dlp", root);

    assert(mkdir(data, 0755) == 0);
    assert(mkdir(output, 0755) == 0);
    write_wav(input);
    write_fake_ytdlp(fake_ytdlp, input);

    DldEngine engine;
    dld_engine_init(&engine);

    DldAppError error;
    dld_app_error_init(&error);

    DldEngineConfig config = {
        .data_dir = data,
        .output_dir = output,
        .yt_dlp = fake_ytdlp,
        .ffmpeg = ffmpeg_path,
        .ffprobe = ffprobe_path,
        .youtube_protection = true,
    };

    assert(dld_engine_open(&engine, &config, &error));

    test_conversion(&engine, input, &error);
    dld_app_error_clear(&error);

    test_resume_playlist_after_failure(&engine, output, &error);
    dld_app_error_clear(&error);

    /* Reescreve o fake para duas faixas válidas; a partir daqui os testes usam
     * esse comportamento. */
    write_both_fake_ytdlp(fake_ytdlp, input);

    test_playlist_redownload_no_duplicates(&engine, output, &error);
    dld_app_error_clear(&error);

    dld_engine_clear(&engine);
    cleanup_test_tree(root, data, output, input, fake_ytdlp);

    free(ffmpeg_path);
    free(ffprobe_path);

    puts("test_engine: ok");
    return 0;
}
