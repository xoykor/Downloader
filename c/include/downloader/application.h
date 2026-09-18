#ifndef DOWNLOADER_APPLICATION_H
#define DOWNLOADER_APPLICATION_H

/*
 * Máquina de estados e fila em memória.
 *
 * A fila guarda índices do vetor `tasks`, não ponteiros. O vetor pode crescer com
 * `realloc` sem invalidar a ordem pendente, e tarefas nunca são removidas durante
 * a vida de `DldApplicationState`, portanto seus índices permanecem estáveis.
 */

#include "downloader/domain.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t max_downloads;
    size_t max_conversions;
} DldQueueLimits;

typedef struct {
    const char *task_id;
    uint64_t sequence;
    DldTaskStatus status;
    const char *message;
    const char *destination;
} DldTaskEvent;

typedef enum {
    DLD_START_NONE = 0,
    DLD_START_STARTED,
    DLD_START_ERROR
} DldStartResult;

typedef struct {
    DldTaskRecord *tasks;
    size_t task_count;
    size_t task_capacity;

    size_t *pending_indices;
    size_t pending_count;
    size_t pending_capacity;

    DldQueueLimits limits;
    size_t active_downloads;
    size_t active_conversions;
    uint64_t next_sequence;
} DldApplicationState;

DldQueueLimits dld_queue_limits_default(void);
void dld_application_state_init(DldApplicationState *state, DldQueueLimits limits);
void dld_application_state_clear(DldApplicationState *state);

size_t dld_application_task_count(const DldApplicationState *state);
const DldTaskRecord *dld_application_task_at(const DldApplicationState *state, size_t index);
DldTaskRecord *dld_application_find_task_mut(DldApplicationState *state, const char *id);
const DldTaskRecord *dld_application_find_task(const DldApplicationState *state, const char *id);

bool dld_application_restore(DldApplicationState *state, const DldTaskRecord *task,
                             DldAppError *error);
bool dld_application_enqueue(DldApplicationState *state, const DldTaskRecord *task,
                             DldTaskEvent *event, DldAppError *error);
size_t dld_application_recover_after_restart(DldApplicationState *state);
bool dld_application_cancel(DldApplicationState *state, const char *id,
                            DldTaskEvent *event, DldAppError *error);
bool dld_application_retry(DldApplicationState *state, const char *id,
                           DldTaskEvent *event, DldAppError *error);
DldStartResult dld_application_start_next(DldApplicationState *state,
                                          DldTaskEvent *event, DldAppError *error);
bool dld_application_finish(DldApplicationState *state, const char *id,
                            DldTaskStatus final_status, const DldAppError *task_error,
                            DldTaskEvent *event, DldAppError *error);

#ifdef __cplusplus
}
#endif

#endif
