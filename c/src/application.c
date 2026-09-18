#include "downloader/application.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t dld_now_ms(void)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return 0;
    }

    return ((uint64_t)now.tv_sec * 1000U) + ((uint64_t)now.tv_nsec / 1000000U);
}

static void dld_set_internal_error(DldAppError *error, const char *message, const char *step)
{
    if (error == NULL) {
        return;
    }

    /*
     * Em falta extrema de memória, até copiar a mensagem pode falhar. Nesse
     * caso a função principal ainda retorna false; o erro textual é best-effort.
     */
    (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, message, step, false, 0);
}

static bool dld_reserve_tasks(DldApplicationState *state, size_t required)
{
    if (required <= state->task_capacity) {
        return true;
    }

    size_t new_capacity = state->task_capacity == 0 ? 8U : state->task_capacity * 2U;
    while (new_capacity < required) {
        new_capacity *= 2U;
    }

    DldTaskRecord *grown = realloc(state->tasks, new_capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }

    state->tasks = grown;
    state->task_capacity = new_capacity;
    return true;
}

static bool dld_reserve_pending(DldApplicationState *state, size_t required)
{
    if (required <= state->pending_capacity) {
        return true;
    }

    size_t new_capacity = state->pending_capacity == 0 ? 8U : state->pending_capacity * 2U;
    while (new_capacity < required) {
        new_capacity *= 2U;
    }

    size_t *grown = realloc(state->pending_indices, new_capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }

    state->pending_indices = grown;
    state->pending_capacity = new_capacity;
    return true;
}

static size_t dld_find_task_index(const DldApplicationState *state, const char *id)
{
    if (state == NULL || id == NULL) {
        return SIZE_MAX;
    }

    for (size_t index = 0; index < state->task_count; ++index) {
        const DldTaskRecord *task = &state->tasks[index];
        if (task->id != NULL && strcmp(task->id, id) == 0) {
            return index;
        }
    }

    return SIZE_MAX;
}

static bool dld_push_pending(DldApplicationState *state, size_t task_index)
{
    if (!dld_reserve_pending(state, state->pending_count + 1U)) {
        return false;
    }

    state->pending_indices[state->pending_count++] = task_index;
    return true;
}

static void dld_remove_pending_position(DldApplicationState *state, size_t position)
{
    if (position >= state->pending_count) {
        return;
    }

    const size_t elements_after = state->pending_count - position - 1U;
    if (elements_after > 0U) {
        memmove(
            &state->pending_indices[position],
            &state->pending_indices[position + 1U],
            elements_after * sizeof(state->pending_indices[0])
        );
    }

    --state->pending_count;
}

static void dld_remove_task_from_pending(DldApplicationState *state, size_t task_index)
{
    size_t write_index = 0;
    for (size_t read_index = 0; read_index < state->pending_count; ++read_index) {
        if (state->pending_indices[read_index] != task_index) {
            state->pending_indices[write_index++] = state->pending_indices[read_index];
        }
    }
    state->pending_count = write_index;
}

static bool dld_can_start(const DldApplicationState *state, size_t task_index)
{
    const DldTaskRecord *task = &state->tasks[task_index];

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

static DldTaskStatus dld_running_status_for_kind(DldTaskKind kind)
{
    switch (kind) {
    case DLD_TASK_DOWNLOAD:
        return DLD_STATUS_DOWNLOADING;
    case DLD_TASK_MERGE:
        return DLD_STATUS_MERGING;
    case DLD_TASK_CONVERT:
        return DLD_STATUS_CONVERTING;
    case DLD_TASK_VALIDATE:
        return DLD_STATUS_VALIDATING;
    default:
        return DLD_STATUS_FAILED;
    }
}

static void dld_fill_event(
    DldApplicationState *state,
    const DldTaskRecord *task,
    const char *message,
    DldTaskEvent *event
)
{
    if (event == NULL) {
        return;
    }

    ++state->next_sequence;
    event->task_id = task->id;
    event->sequence = state->next_sequence;
    event->status = task->status;
    event->message = message;
    event->destination = task->destination;
}

DldQueueLimits dld_queue_limits_default(void)
{
    DldQueueLimits limits = {
        .max_downloads = 2U,
        .max_conversions = 1U,
    };
    return limits;
}

void dld_application_state_init(DldApplicationState *state, DldQueueLimits limits)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->limits = limits;
}

void dld_application_state_clear(DldApplicationState *state)
{
    if (state == NULL) {
        return;
    }

    for (size_t index = 0; index < state->task_count; ++index) {
        dld_task_record_clear(&state->tasks[index]);
    }

    free(state->tasks);
    free(state->pending_indices);

    DldQueueLimits limits = state->limits;
    dld_application_state_init(state, limits);
}

size_t dld_application_task_count(const DldApplicationState *state)
{
    return state == NULL ? 0U : state->task_count;
}

const DldTaskRecord *dld_application_task_at(const DldApplicationState *state, size_t index)
{
    if (state == NULL || index >= state->task_count) {
        return NULL;
    }
    return &state->tasks[index];
}

const DldTaskRecord *dld_application_find_task(const DldApplicationState *state, const char *id)
{
    const size_t index = dld_find_task_index(state, id);
    return index == SIZE_MAX ? NULL : &state->tasks[index];
}

bool dld_application_restore(
    DldApplicationState *state,
    const DldTaskRecord *task,
    DldAppError *error
)
{
    if (state == NULL || task == NULL || task->id == NULL) {
        dld_set_internal_error(error, "Registro de tarefa inválido.", "persistência");
        return false;
    }

    if (dld_find_task_index(state, task->id) != SIZE_MAX) {
        dld_set_internal_error(error, "ID de tarefa duplicado no armazenamento.", "persistência");
        return false;
    }

    if (!dld_reserve_tasks(state, state->task_count + 1U)) {
        dld_set_internal_error(error, "Memória insuficiente para restaurar tarefa.", "persistência");
        return false;
    }

    if (task->status == DLD_STATUS_QUEUED &&
        !dld_reserve_pending(state, state->pending_count + 1U)) {
        dld_set_internal_error(error, "Memória insuficiente para restaurar a fila.", "persistência");
        return false;
    }

    DldTaskRecord copy;
    dld_task_record_init(&copy);
    if (!dld_task_record_copy(&copy, task)) {
        dld_set_internal_error(error, "Memória insuficiente para copiar tarefa.", "persistência");
        return false;
    }

    const size_t new_index = state->task_count;
    state->tasks[state->task_count++] = copy;

    if (task->status == DLD_STATUS_QUEUED) {
        state->pending_indices[state->pending_count++] = new_index;
    }

    return true;
}

bool dld_application_enqueue(
    DldApplicationState *state,
    const DldTaskRecord *task,
    DldTaskEvent *event,
    DldAppError *error
)
{
    if (state == NULL || task == NULL || task->id == NULL || task->id[0] == '\0') {
        dld_set_internal_error(error, "Tarefa sem ID.", "fila");
        return false;
    }

    if (dld_find_task_index(state, task->id) != SIZE_MAX) {
        dld_set_internal_error(error, "ID de tarefa já existe.", "fila");
        return false;
    }

    /* Reserva tudo antes de alterar o estado; assim falhas de memória não deixam meia tarefa. */
    if (!dld_reserve_tasks(state, state->task_count + 1U) ||
        !dld_reserve_pending(state, state->pending_count + 1U)) {
        dld_set_internal_error(error, "Memória insuficiente para adicionar tarefa.", "fila");
        return false;
    }

    DldTaskRecord copy;
    dld_task_record_init(&copy);
    if (!dld_task_record_copy(&copy, task)) {
        dld_set_internal_error(error, "Memória insuficiente para copiar tarefa.", "fila");
        return false;
    }

    copy.status = DLD_STATUS_QUEUED;
    copy.updated_at_ms = dld_now_ms();

    const size_t new_index = state->task_count;
    state->tasks[state->task_count++] = copy;
    state->pending_indices[state->pending_count++] = new_index;

    dld_fill_event(state, &state->tasks[new_index], "Tarefa adicionada à fila.", event);
    return true;
}

size_t dld_application_recover_after_restart(DldApplicationState *state)
{
    if (state == NULL) {
        return 0U;
    }

    size_t recovered = 0U;
    const uint64_t now = dld_now_ms();

    for (size_t index = 0; index < state->task_count; ++index) {
        DldTaskRecord *task = &state->tasks[index];
        if (dld_task_status_is_active(task->status)) {
            task->status = DLD_STATUS_INTERRUPTED;
            task->updated_at_ms = now;
            ++recovered;
        }
    }

    /* Um estado recém-aberto não possui processos ativos, mesmo que o banco dissesse o contrário. */
    state->active_downloads = 0U;
    state->active_conversions = 0U;
    return recovered;
}

bool dld_application_cancel(
    DldApplicationState *state,
    const char *id,
    DldTaskEvent *event,
    DldAppError *error
)
{
    const size_t index = dld_find_task_index(state, id);
    if (index == SIZE_MAX) {
        dld_set_internal_error(error, "Tarefa não encontrada.", "fila");
        return false;
    }

    DldTaskRecord *task = &state->tasks[index];
    if (dld_task_status_is_terminal(task->status)) {
        dld_set_internal_error(error, "A tarefa já terminou.", "fila");
        return false;
    }

    task->status = DLD_STATUS_CANCELLED;
    task->updated_at_ms = dld_now_ms();
    dld_remove_task_from_pending(state, index);

    /*
     * Se um processo já estava ativo, os contadores só são liberados em
     * `finish`, depois que o executor confirmar que o processo realmente saiu.
     * Isso reproduz a semântica segura da implementação Rust.
     */
    dld_fill_event(state, task, "Cancelamento solicitado.", event);
    return true;
}

bool dld_application_retry(
    DldApplicationState *state,
    const char *id,
    DldTaskEvent *event,
    DldAppError *error
)
{
    const size_t index = dld_find_task_index(state, id);
    if (index == SIZE_MAX) {
        dld_set_internal_error(error, "Tarefa não encontrada.", "fila");
        return false;
    }

    DldTaskRecord *task = &state->tasks[index];
    const bool retryable =
        task->status == DLD_STATUS_FAILED ||
        task->status == DLD_STATUS_CANCELLED ||
        task->status == DLD_STATUS_INTERRUPTED;

    if (!retryable) {
        dld_set_internal_error(
            error,
            "Só tarefas falhas, canceladas ou interrompidas podem ser repetidas.",
            "fila"
        );
        return false;
    }

    if (!dld_push_pending(state, index)) {
        dld_set_internal_error(error, "Memória insuficiente para recolocar tarefa na fila.", "fila");
        return false;
    }

    task->status = DLD_STATUS_QUEUED;
    task->has_error = false;
    dld_app_error_clear(&task->error);
    task->updated_at_ms = dld_now_ms();

    dld_fill_event(state, task, "Tarefa retornou à fila.", event);
    return true;
}

DldStartResult dld_application_start_next(
    DldApplicationState *state,
    DldTaskEvent *event,
    DldAppError *error
)
{
    if (state == NULL) {
        dld_set_internal_error(error, "Estado da aplicação inválido.", "fila");
        return DLD_START_ERROR;
    }

    size_t pending_position = SIZE_MAX;
    for (size_t position = 0; position < state->pending_count; ++position) {
        const size_t task_index = state->pending_indices[position];
        if (task_index < state->task_count && dld_can_start(state, task_index)) {
            pending_position = position;
            break;
        }
    }

    if (pending_position == SIZE_MAX) {
        return DLD_START_NONE;
    }

    const size_t task_index = state->pending_indices[pending_position];
    dld_remove_pending_position(state, pending_position);

    DldTaskRecord *task = &state->tasks[task_index];
    task->status = dld_running_status_for_kind(task->kind);
    task->updated_at_ms = dld_now_ms();

    if (task->kind == DLD_TASK_DOWNLOAD) {
        ++state->active_downloads;
    } else if (task->kind == DLD_TASK_CONVERT || task->kind == DLD_TASK_MERGE) {
        ++state->active_conversions;
    }

    dld_fill_event(state, task, "Tarefa iniciada.", event);
    return DLD_START_STARTED;
}

bool dld_application_finish(
    DldApplicationState *state,
    const char *id,
    DldTaskStatus final_status,
    const DldAppError *task_error,
    DldTaskEvent *event,
    DldAppError *error
)
{
    const bool valid_final_status =
        final_status == DLD_STATUS_COMPLETED ||
        final_status == DLD_STATUS_FAILED ||
        final_status == DLD_STATUS_CANCELLED;

    if (!valid_final_status) {
        dld_set_internal_error(error, "Estado final inválido.", "fila");
        return false;
    }

    const size_t index = dld_find_task_index(state, id);
    if (index == SIZE_MAX) {
        dld_set_internal_error(error, "Tarefa não encontrada.", "fila");
        return false;
    }

    DldTaskRecord *task = &state->tasks[index];

    /* Copia o erro antes de mudar contadores/estado para manter a operação transacional. */
    DldAppError copied_error;
    dld_app_error_init(&copied_error);
    if (task_error != NULL && !dld_app_error_copy(&copied_error, task_error)) {
        dld_set_internal_error(error, "Memória insuficiente para registrar erro da tarefa.", "fila");
        return false;
    }

    if (task->kind == DLD_TASK_DOWNLOAD && state->active_downloads > 0U) {
        --state->active_downloads;
    } else if ((task->kind == DLD_TASK_CONVERT || task->kind == DLD_TASK_MERGE) &&
               state->active_conversions > 0U) {
        --state->active_conversions;
    }

    dld_app_error_clear(&task->error);
    task->has_error = task_error != NULL;
    if (task_error != NULL) {
        task->error = copied_error;
    }

    task->status = final_status;
    task->updated_at_ms = dld_now_ms();
    dld_fill_event(state, task, NULL, event);
    return true;
}
