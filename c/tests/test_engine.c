#include "downloader/engine.h"
#include "downloader/process.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void le16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xffU), f);
    fputc((int)((v >> 8U) & 0xffU), f);
}

static void le32(FILE *f, uint32_t v)
{
    for (unsigned i = 0U; i < 4U; ++i) fputc((int)((v >> (8U * i)) & 0xffU), f);
}

static void write_wav(const char *path)
{
    const uint32_t samples = 800U;
    const uint32_t data_size = samples * 2U;
    FILE *f = fopen(path, "wb"); assert(f != NULL);
    fwrite("RIFF", 1U, 4U, f); le32(f, 36U + data_size); fwrite("WAVE", 1U, 4U, f);
    fwrite("fmt ", 1U, 4U, f); le32(f, 16U); le16(f, 1U); le16(f, 1U);
    le32(f, 8000U); le32(f, 16000U); le16(f, 2U); le16(f, 16U);
    fwrite("data", 1U, 4U, f); le32(f, data_size);
    for (uint32_t i = 0U; i < samples; ++i) le16(f, 0U);
    fclose(f);
}

int main(void)
{
    char *ffmpeg_path = NULL, *ffprobe_path = NULL;
    if (!dld_process_detect("ffmpeg", &ffmpeg_path, NULL) ||
        !dld_process_detect("ffprobe", &ffprobe_path, NULL)) {
        free(ffmpeg_path); free(ffprobe_path);
        puts("test_engine: skipped (ffmpeg/ffprobe ausentes)");
        return 0;
    }
    char root[] = "/tmp/downloader-engine-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char data[512], output[512], input[512];
    snprintf(data, sizeof(data), "%s/data", root);
    snprintf(output, sizeof(output), "%s/out", root);
    snprintf(input, sizeof(input), "%s/input.wav", root);
    assert(mkdir(data, 0755) == 0);
    assert(mkdir(output, 0755) == 0);
    write_wav(input);

    DldEngine engine; dld_engine_init(&engine);
    DldAppError error; dld_app_error_init(&error);
    DldEngineConfig config = {
        .data_dir = data, .output_dir = output,
        .yt_dlp = "yt-dlp", .ffmpeg = ffmpeg_path, .ffprobe = ffprobe_path,
        .youtube_protection = false,
    };
    assert(dld_engine_open(&engine, &config, &error));
    DldTaskRecord task; dld_task_record_init(&task);
    task.id = dld_string_duplicate("convert-test");
    task.kind = DLD_TASK_CONVERT;
    task.input_path = dld_string_duplicate(input);
    task.options_json = dld_string_duplicate("{\"formato\":\"mp3\",\"aceleracao\":\"software\"}");
    task.collision = DLD_COLLISION_RENAME;
    atomic_bool cancel = false;
    assert(dld_engine_execute_task(&engine, &task, &cancel, NULL, NULL, &error));
    assert(task.status == DLD_STATUS_COMPLETED);
    assert(task.destination != NULL && access(task.destination, F_OK) == 0);

    unlink(task.destination); unlink(input);
    char db[512]; snprintf(db, sizeof(db), "%s/tasks.sqlite", data); unlink(db);
    char wal[520]; snprintf(wal, sizeof(wal), "%s-wal", db); unlink(wal);
    char shm[520]; snprintf(shm, sizeof(shm), "%s-shm", db); unlink(shm);
    char tmpdir[512]; snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", data); rmdir(tmpdir);
    rmdir(output); rmdir(data); rmdir(root);
    dld_task_record_clear(&task); dld_engine_clear(&engine); dld_app_error_clear(&error);
    free(ffmpeg_path); free(ffprobe_path);
    puts("test_engine: ok");
    return 0;
}
