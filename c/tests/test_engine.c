#include "downloader/engine.h"
#include "downloader/process.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    size_t child_progress_events;
    size_t child_completed_events;
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
 * - a segunda faixa "falha", fazendo o processo terminar com código 1.
 *
 * A engine deve conservar/publicar a faixa válida em vez de descartar tudo.
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
    fputs("for arg in \"$@\"; do\n", script);
    fputs("  if [ \"$previous\" = \"-o\" ]; then output=\"$arg\"; break; fi\n", script);
    fputs("  previous=\"$arg\"\n", script);
    fputs("done\n", script);
    fputs("dir=${output%/*}\n", script);

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

    fputs("printf '%s\\n' 'item id2 indisponível' >&2\n", script);
    fputs("exit 1\n", script);

    assert(fclose(script) == 0);
    assert(chmod(path, 0755) == 0);
}

static void capture_event(const DldEngineEvent *event, void *userdata)
{
    EventStats *stats = userdata;
    if (event == NULL || event->task_id == NULL) return;
    if (strstr(event->task_id, "::") == NULL) return;

    if (event->has_progress) {
        ++stats->child_progress_events;
    }
    if (event->status == DLD_STATUS_COMPLETED) {
        ++stats->child_completed_events;
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

static void test_partial_playlist(DldEngine *engine,
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
    assert(stats.child_progress_events > 0U);
    assert(stats.child_completed_events > 0U);

    unsigned youtube_starts = 0U;
    uint64_t oldest_start = 0U;
    assert(dld_database_count_youtube_starts_since(
        &engine->database,
        0U,
        &youtube_starts,
        &oldest_start,
        error));
    assert(youtube_starts == 1U);
    assert(oldest_start > 0U);

    char published[768];
    (void)snprintf(
        published,
        sizeof(published),
        "%s/Faixa Um [id1].wav",
        output);
    assert(access(published, F_OK) == 0);

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

    test_partial_playlist(&engine, output, &error);
    dld_app_error_clear(&error);

    dld_engine_clear(&engine);
    cleanup_test_tree(root, data, output, input, fake_ytdlp);

    free(ffmpeg_path);
    free(ffprobe_path);

    puts("test_engine: ok");
    return 0;
}
