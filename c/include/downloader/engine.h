#ifndef DOWNLOADER_ENGINE_H
#define DOWNLOADER_ENGINE_H

#include "downloader/database.h"
#include "downloader/domain.h"
#include "downloader/media.h"

#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *data_dir;
    const char *output_dir;
    const char *yt_dlp;
    const char *ffmpeg;
    const char *ffprobe;
    bool youtube_protection;
} DldEngineConfig;

typedef struct {
    char *data_dir;
    char *output_dir;
    char *yt_dlp;
    char *ffmpeg;
    char *ffprobe;
    bool youtube_protection;
    DldDatabase database;
} DldEngine;

typedef struct {
    const char *task_id;
    DldTaskStatus status;
    bool has_progress;
    double progress_percent;
    const char *speed;
    const char *message;
    const char *path;
} DldEngineEvent;

typedef void (*DldEngineEventCallback)(const DldEngineEvent *event, void *userdata);

DldEngineConfig dld_engine_config_default(void);
void dld_engine_init(DldEngine *engine);
void dld_engine_clear(DldEngine *engine);
bool dld_engine_open(DldEngine *engine, const DldEngineConfig *config, DldAppError *error);

bool dld_engine_check_dependencies(DldEngine *engine, char **report, DldAppError *error);

bool dld_engine_analyze(DldEngine *engine, const char *url, bool playlist,
                        const DldAuthRef *auth, atomic_bool *cancel_flag,
                        DldMediaSummary *summary, DldAppError *error);

/* Executa Download/Convert/Validate e persiste todas as transições da tarefa. */
bool dld_engine_execute_task(DldEngine *engine, DldTaskRecord *task,
                             atomic_bool *cancel_flag,
                             DldEngineEventCallback callback, void *userdata,
                             DldAppError *error);

#ifdef __cplusplus
}
#endif

#endif
