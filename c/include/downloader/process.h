#ifndef DOWNLOADER_PROCESS_H
#define DOWNLOADER_PROCESS_H

#include "downloader/domain.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *program;
    char *const *argv;          /* argv[0] deve ser program; vetor terminado em NULL. */
    const char *working_directory;
    unsigned timeout_ms;        /* 0 = sem timeout. */
} DldProcessSpec;

typedef struct {
    bool exited;
    int exit_code;
    bool timed_out;
    bool cancelled;
    char *stdout_text;
    char *stderr_text;
} DldProcessResult;

typedef void (*DldProcessLineCallback)(const char *line, void *userdata);

void dld_process_result_init(DldProcessResult *result);
void dld_process_result_clear(DldProcessResult *result);

/*
 * Executa diretamente com execvp, nunca por shell. Assim URLs, nomes de arquivo
 * e cookies permanecem argumentos independentes e não viram código de shell.
 */
bool dld_process_run(const DldProcessSpec *spec, atomic_bool *cancel_flag,
                     DldProcessLineCallback on_stdout_line, void *userdata,
                     DldProcessResult *result, DldAppError *error);

/* Resolve PATH e opcionalmente captura a primeira linha de --version. */
bool dld_process_detect(const char *program, char **resolved_path, char **version_line);

#ifdef __cplusplus
}
#endif

#endif
