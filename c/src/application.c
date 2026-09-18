/*
 * Fila e máquina de estados em memória.
 *
 * Invariante principal: `tasks` cresce, mas tarefas não são removidas. Por isso
 * `pending_indices` pode guardar índices estáveis mesmo quando `tasks` sofre
 * `realloc`. Downloads e conversões possuem limites de concorrência separados.
 */

#include "downloader/application.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0;
    return ((uint64_t)now.tv_sec * UINT64_C(1000)) +
           ((uint64_t)now.tv_nsec / UINT64_C(1000000));
}

static void set_internal_error(DldAppError *error, const char *message, const char *step)
{
    if (error != NULL) {
        (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, message, step, false, 0);
    }
}

/* Crescimento geométrico evita `realloc` a cada nova tarefa. */
static bool reserve_tasks(DldApplicationState *state, size_t required)
{
    if (required <= state->task_capacity) return true;
    size_t capacity = state->task_capacity == 0U ? 8U : state->task_capacity * 2U;
    while (capacity < required) capacity *= 2U;
    DldTaskRecord *grown = realloc(state->tasks, capacity * sizeof(*grown));
    if (grown == NULL) return false;
    state->tasks = grown;
    state->task_capacity = capacity;
    return true;
}

/* A fila pendente armazena índices estáveis do vetor `tasks`. */
static bool reserve_pending(DldApplicationState *state, size_t required)
{
    if (required <= state->pending_capacity) return true;
    size_t capacity = state->pending_capacity == 0U ? 8U : state->pending_capacity * 2U;
    while (capacity < required) capacity *= 2U;
    size_t *grown = realloc(state->pending_indices, capacity * sizeof(*grown));
    if (grown == NULL) return false;
    state->pending_indices = grown;
    state->pending_capacity = capacity;
    return true;
}

static size_t find_index(const DldApplicationState *state, const char *id)
{
    if (state == NULL || id == NULL) return SIZE_MAX;
    for (size_t i = 0; i < state->task_count; ++i) {
        if (state->tasks[i].id != NULL && strcmp(state->tasks[i].id, id) == 0) return i;
    }
    return SIZE_MAX;
}

static bool push_pending(DldApplicationState *state, size_t task_index)
{
    if (!reserve_pending(state, state->pending_count + 1U)) return false;
    state->pending_indices[state->pending_count++] = task_index;
    return true;
}

static void remove_pending_position(DldApplicationState *state, size_t position)
{
    if (position >= state->pending_count) return;
    const size_t after = state->pending_count - position - 1U;
    if (after > 0U) {
        memmove(&state->pending_indices[position], &state->pending_indices[position + 1U],
                after * sizeof(state->pending_indices[0]));
    }
    --state->pending_count;
}

static void remove_task_from_pending(DldApplicationState *state, size_t task_index)
{
    size_t write = 0U;
    for (size_t read = 0U; read < state->pending_count; ++read) {
        if (state->pending_indices[read] != task_index) {
            state->pending_indices[write++] = state->pending_indices[read];
        }
    }
    state->pending_count = write;
}

static bool can_start(const DldApplicationState *state, size_t index)
{
    const DldTaskRecord *task = &state->tasks[index];
    if (task->status != DLD_STATUS_QUEUED) return false;
    switch (task->kind) {
    case DLD_TASK_DOWNLOAD:
        return state->active_downloads < state->limits.max_downloads;
    case DLD_TASK_CONVERT:
    case DLD_TASK_MERGE:
        return state->active_conversions < state->limits.max_conversions;
    case DLD_TASK_VALIDATE:
        return true;
    default:
        return false;
    }
}

static DldTaskStatus running_status(DldTaskKind kind)
{
    switch (kind) {
    case DLD_TASK_DOWNLOAD: return DLD_STATUS_DOWNLOADING;
    case DLD_TASK_MERGE: return DLD_STATUS_MERGING;
    case DLD_TASK_CONVERT: return DLD_STATUS_CONVERTING;
    case DLD_TASK_VALIDATE: return DLD_STATUS_VALIDATING;
    default: return DLD_STATUS_FAILED;
    }
}

static void fill_event(DldApplicationState *state, const DldTaskRecord *task,
                       const char *message, DldTaskEvent *event)
{
    if (event == NULL) return;
    ++state->next_sequence;
    event->task_id = task->id;
    event->sequence = state->next_sequence;
    event->status = task->status;
    event->message = message;
    event->destination = task->destination;
}

DldQueueLimits dld_queue_limits_default(void)
{
    return (DldQueueLimits){.max_downloads = 2U, .max_conversions = 1U};
}

void dld_application_state_init(DldApplicationState *state, DldQueueLimits limits)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->limits = limits;
}

void dld_application_state_clear(DldApplicationState *state)
{
    if (state == NULL) return;
    for (size_t i = 0; i < state->task_count; ++i) {
        dld_task_record_clear(&state->tasks[i]);
    }
    free(state->tasks);
    free(state->pending_indices);
    const DldQueueLimits limits = state->limits;
    dld_application_state_init(state, limits);
}

size_t dld_application_task_count(const DldApplicationState *state)
{
    return state == NULL ? 0U : state->task_count;
}

const DldTaskRecord *dld_application_task_at(const DldApplicationState *state, size_t index)
{
    return state != NULL && index < state->task_count ? &state->tasks[index] : NULL;
}

DldTaskRecord *dld_application_find_task_mut(DldApplicationState *state, const char *id)
{
    const size_t index = find_index(state, id);
    return index == SIZE_MAX ? NULL : &state->tasks[index];
}

const DldTaskRecord *dld_application_find_task(const DldApplicationState *state, const char *id)
{
    const size_t index = find_index(state, id);
    return index == SIZE_MAX ? NULL : &state->tasks[index];
}

bool dld_application_restore(DldApplicationState *state, const DldTaskRecord *task,
                             DldAppError *error)
{
    if (state == NULL || task == NULL || task->id == NULL) {
        set_internal_error(error, "Registro de tarefa inválido.", "persistência");
        return false;
    }
    if (find_index(state, task->id) != SIZE_MAX) {
        set_internal_error(error, "ID de tarefa duplicado no armazenamento.", "persistência");
        return false;
    }
    if (!reserve_tasks(state, state->task_count + 1U) ||
        (task->status == DLD_STATUS_QUEUED && !reserve_pending(state, state->pending_count + 1U))) {
        set_internal_error(error, "Memória insuficiente para restaurar tarefa.", "persistência");
        return false;
    }
    DldTaskRecord copy;
    dld_task_record_init(&copy);
    if (!dld_task_record_copy(&copy, task)) {
        set_internal_error(error, "Memória insuficiente para copiar tarefa.", "persistência");
        return false;
    }
    const size_t index = state->task_count;
    state->tasks[state->task_count++] = copy;
    if (task->status == DLD_STATUS_QUEUED) {
        state->pending_indices[state->pending_count++] = index;
    }
    return true;
}

bool dld_application_enqueue(DldApplicationState *state, const DldTaskRecord *task,
                             DldTaskEvent *event, DldAppError *error)
{
    if (state == NULL || task == NULL || task->id == NULL || task->id[0] == '\0') {
        set_internal_error(error, "Tarefa sem ID.", "fila");
        return false;
    }
    if (find_index(state, task->id) != SIZE_MAX) {
        set_internal_error(error, "ID de tarefa já existe.", "fila");
        return false;
    }
    /*
     * Reserve as duas estruturas antes de alterar qualquer contador. Assim uma
     * falha de memória nunca deixa uma tarefa inserida pela metade na fila.
     */
    if (!reserve_tasks(state, state->task_count + 1U) ||
        !reserve_pending(state, state->pending_count + 1U)) {
        set_internal_error(error, "Memória insuficiente para adicionar tarefa.", "fila");
        return false;
    }
    DldTaskRecord copy;
    dld_task_record_init(&copy);
    if (!dld_task_record_copy(&copy, task)) {
        set_internal_error(error, "Memória insuficiente para copiar tarefa.", "fila");
        return false;
    }
    copy.status = DLD_STATUS_QUEUED;
    copy.updated_at_ms = now_ms();
    const size_t index = state->task_count;
    state->tasks[state->task_count++] = copy;
    state->pending_indices[state->pending_count++] = index;
    fill_event(state, &state->tasks[index], "Tarefa adicionada à fila.", event);
    return true;
}

/*
 * Não existe processo sobrevivente associado ao estado restaurado. Portanto um
 * estado que era "ativo" antes do encerramento precisa virar "interrompido";
 * tratá-lo como ainda ativo bloquearia a fila para sempre após reiniciar.
 */
size_t dld_application_recover_after_restart(DldApplicationState *state)
{
    if (state == NULL) return 0U;
    size_t recovered = 0U;
    const uint64_t timestamp = now_ms();
    for (size_t i = 0; i < state->task_count; ++i) {
        if (dld_task_status_is_active(state->tasks[i].status)) {
            state->tasks[i].status = DLD_STATUS_INTERRUPTED;
            state->tasks[i].updated_at_ms = timestamp;
            ++recovered;
        }
    }
    state->active_downloads = 0U;
    state->active_conversions = 0U;
    return recovered;
}

bool dld_application_cancel(DldApplicationState *state, const char *id,
                            DldTaskEvent *event, DldAppError *error)
{
    const size_t index = find_index(state, id);
    if (index == SIZE_MAX) {
        set_internal_error(error, "Tarefa não encontrada.", "fila");
        return false;
    }
    DldTaskRecord *task = &state->tasks[index];
    if (dld_task_status_is_terminal(task->status)) {
        set_internal_error(error, "A tarefa já terminou.", "fila");
        return false;
    }
    task->status = DLD_STATUS_CANCELLED;
    task->updated_at_ms = now_ms();
    remove_task_from_pending(state, index);
    fill_event(state, task, "Cancelamento solicitado.", event);
    return true;
}

bool dld_application_retry(DldApplicationState *state, const char *id,
                           DldTaskEvent *event, DldAppError *error)
{
    const size_t index = find_index(state, id);
    if (index == SIZE_MAX) {
        set_internal_error(error, "Tarefa não encontrada.", "fila");
        return false;
    }
    DldTaskRecord *task = &state->tasks[index];
    if (task->status != DLD_STATUS_FAILED && task->status != DLD_STATUS_CANCELLED &&
        task->status != DLD_STATUS_INTERRUPTED) {
        set_internal_error(error, "Só tarefas falhas, canceladas ou interrompidas podem ser repetidas.", "fila");
        return false;
    }
    if (!push_pending(state, index)) {
        set_internal_error(error, "Memória insuficiente para recolocar tarefa na fila.", "fila");
        return false;
    }
    task->status = DLD_STATUS_QUEUED;
    task->has_error = false;
    dld_app_error_clear(&task->error);
    task->updated_at_ms = now_ms();
    fill_event(state, task, "Tarefa retornou à fila.", event);
    return true;
}

DldStartResult dld_application_start_next(DldApplicationState *state,
                                          DldTaskEvent *event, DldAppError *error)
{
    if (state == NULL) {
        set_internal_error(error, "Estado da aplicação inválido.", "fila");
        return DLD_START_ERROR;
    }
    /*
     * Procura da esquerda para a direita para preservar FIFO. Uma tarefa que não
     * cabe no limite atual não impede outra categoria independente de iniciar.
     */
    size_t position = SIZE_MAX;
    for (size_t i = 0; i < state->pending_count; ++i) {
        const size_t index = state->pending_indices[i];
        if (index < state->task_count && can_start(state, index)) {
            position = i;
            break;
        }
    }
    if (position == SIZE_MAX) return DLD_START_NONE;

    const size_t index = state->pending_indices[position];
    remove_pending_position(state, position);
    DldTaskRecord *task = &state->tasks[index];
    task->status = running_status(task->kind);
    task->updated_at_ms = now_ms();
    if (task->kind == DLD_TASK_DOWNLOAD) {
        ++state->active_downloads;
    }
    if (task->kind == DLD_TASK_CONVERT || task->kind == DLD_TASK_MERGE) {
        ++state->active_conversions;
    }
    fill_event(state, task, "Tarefa iniciada.", event);
    return DLD_START_STARTED;
}

bool dld_application_finish(DldApplicationState *state, const char *id,
                            DldTaskStatus final_status, const DldAppError *task_error,
                            DldTaskEvent *event, DldAppError *error)
{
    if (final_status != DLD_STATUS_COMPLETED && final_status != DLD_STATUS_FAILED &&
        final_status != DLD_STATUS_CANCELLED) {
        set_internal_error(error, "Estado final inválido.", "fila");
        return false;
    }
    const size_t index = find_index(state, id);
    if (index == SIZE_MAX) {
        set_internal_error(error, "Tarefa não encontrada.", "fila");
        return false;
    }
    /*
     * O erro recebido pode apontar para memória pertencente ao chamador. Faça a
     * cópia antes de mudar contadores/estado para manter a operação consistente
     * mesmo se faltar memória.
     */
    DldAppError copied;
    dld_app_error_init(&copied);
    if (task_error != NULL && !dld_app_error_copy(&copied, task_error)) {
        set_internal_error(error, "Memória insuficiente para registrar erro da tarefa.", "fila");
        return false;
    }

    DldTaskRecord *task = &state->tasks[index];
    if (task->kind == DLD_TASK_DOWNLOAD && state->active_downloads > 0U) --state->active_downloads;
    if ((task->kind == DLD_TASK_CONVERT || task->kind == DLD_TASK_MERGE) &&
        state->active_conversions > 0U) --state->active_conversions;
    dld_app_error_clear(&task->error);
    task->has_error = task_error != NULL;
    if (task_error != NULL) task->error = copied;
    task->status = final_status;
    task->updated_at_ms = now_ms();
    fill_event(state, task, NULL, event);
    return true;
}
