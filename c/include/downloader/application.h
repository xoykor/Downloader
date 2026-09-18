#ifndef DOWNLOADER_APPLICATION_H
#define DOWNLOADER_APPLICATION_H

#include "downloader/domain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Limites equivalentes aos defaults atuais do Rust: 2 downloads e 1 conversão. */
typedef struct {
    size_t max_downloads;
    size_t max_conversions;
} DldQueueLimits;

/*
 * Evento leve para consumo imediato por CLI/UI.
 *
 * `task_id`, `message` e `destination` são referências emprestadas. O chamador
 * não deve liberá-las nem mantê-las depois que o estado correspondente mudar.
 * Quando a camada de eventos assíncronos for portada, ela poderá fazer uma
 * cópia profunda deste snapshot na fronteira entre threads.
 */
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

/*
 * Estado da fila.
 *
 * As tarefas ficam em um vetor que só cresce enquanto o estado está aberto.
 * A fila pendente guarda índices nesse vetor; por isso uma realocação do vetor
 * não invalida a ordem da fila.
 */
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
const DldTaskRecord *dld_application_find_task(const DldApplicationState *state, const char *id);

/* Restaura uma tarefa persistida sem emitir evento, como a versão Rust. */
bool dld_application_restore(
    DldApplicationState *state,
    const DldTaskRecord *task,
    DldAppError *error
);

/* Copia a tarefa para o estado; o chamador continua dono do registro original. */
bool dld_application_enqueue(
    DldApplicationState *state,
    const DldTaskRecord *task,
    DldTaskEvent *event,
    DldAppError *error
);

/*
 * Marca tarefas que estavam ativas como interrompidas após reinício.
 * Retorna quantas tarefas foram alteradas. Eventos individuais entram numa
 * etapa posterior, junto do barramento assíncrono da aplicação.
 */
size_t dld_application_recover_after_restart(DldApplicationState *state);

bool dld_application_cancel(
    DldApplicationState *state,
    const char *id,
    DldTaskEvent *event,
    DldAppError *error
);

bool dld_application_retry(
    DldApplicationState *state,
    const char *id,
    DldTaskEvent *event,
    DldAppError *error
);

DldStartResult dld_application_start_next(
    DldApplicationState *state,
    DldTaskEvent *event,
    DldAppError *error
);

bool dld_application_finish(
    DldApplicationState *state,
    const char *id,
    DldTaskStatus final_status,
    const DldAppError *task_error,
    DldTaskEvent *event,
    DldAppError *error
);

#ifdef __cplusplus
}
#endif

#endif
