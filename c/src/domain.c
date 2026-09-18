#include "downloader/domain.h"

#include <stdlib.h>
#include <string.h>

/*
 * `strdup` é POSIX, não C17. Esta pequena função mantém o núcleo estritamente
 * C17 e concentra em um único ponto a política de duplicação de strings.
 */
static char *dld_string_duplicate(const char *text)
{
    if (text == NULL) {
        return NULL;
    }

    const size_t length = strlen(text) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

bool dld_task_status_is_terminal(DldTaskStatus status)
{
    return status == DLD_STATUS_COMPLETED ||
           status == DLD_STATUS_FAILED ||
           status == DLD_STATUS_CANCELLED;
}

bool dld_task_status_is_active(DldTaskStatus status)
{
    switch (status) {
    case DLD_STATUS_ANALYZING:
    case DLD_STATUS_WAITING_AUTH:
    case DLD_STATUS_DOWNLOADING:
    case DLD_STATUS_MERGING:
    case DLD_STATUS_CONVERTING:
    case DLD_STATUS_VALIDATING:
        return true;
    default:
        return false;
    }
}

const char *dld_task_kind_name(DldTaskKind kind)
{
    switch (kind) {
    case DLD_TASK_DOWNLOAD:
        return "download";
    case DLD_TASK_MERGE:
        return "merge";
    case DLD_TASK_CONVERT:
        return "convert";
    case DLD_TASK_VALIDATE:
        return "validate";
    default:
        return "unknown";
    }
}

const char *dld_task_status_name(DldTaskStatus status)
{
    switch (status) {
    case DLD_STATUS_QUEUED:
        return "na_fila";
    case DLD_STATUS_ANALYZING:
        return "analisando";
    case DLD_STATUS_WAITING_AUTH:
        return "aguardando_autenticacao";
    case DLD_STATUS_DOWNLOADING:
        return "baixando";
    case DLD_STATUS_MERGING:
        return "unindo";
    case DLD_STATUS_CONVERTING:
        return "convertendo";
    case DLD_STATUS_VALIDATING:
        return "validando";
    case DLD_STATUS_COMPLETED:
        return "concluido";
    case DLD_STATUS_FAILED:
        return "falhou";
    case DLD_STATUS_CANCELLED:
        return "cancelado";
    case DLD_STATUS_INTERRUPTED:
        return "interrompido";
    default:
        return "desconhecido";
    }
}

void dld_app_error_init(DldAppError *error)
{
    if (error == NULL) {
        return;
    }

    error->category = DLD_ERROR_INTERNAL;
    error->message = NULL;
    error->step = NULL;
    error->has_code = false;
    error->code = 0;
}

void dld_app_error_clear(DldAppError *error)
{
    if (error == NULL) {
        return;
    }

    free(error->message);
    free(error->step);
    dld_app_error_init(error);
}

bool dld_app_error_set(
    DldAppError *error,
    DldErrorCategory category,
    const char *message,
    const char *step,
    bool has_code,
    int code
)
{
    if (error == NULL) {
        return false;
    }

    char *message_copy = dld_string_duplicate(message);
    char *step_copy = dld_string_duplicate(step);

    if ((message != NULL && message_copy == NULL) ||
        (step != NULL && step_copy == NULL)) {
        free(message_copy);
        free(step_copy);
        return false;
    }

    dld_app_error_clear(error);
    error->category = category;
    error->message = message_copy;
    error->step = step_copy;
    error->has_code = has_code;
    error->code = code;
    return true;
}

bool dld_app_error_copy(DldAppError *destination, const DldAppError *source)
{
    if (destination == NULL || source == NULL) {
        return false;
    }

    return dld_app_error_set(
        destination,
        source->category,
        source->message,
        source->step,
        source->has_code,
        source->code
    );
}

void dld_task_record_init(DldTaskRecord *task)
{
    if (task == NULL) {
        return;
    }

    memset(task, 0, sizeof(*task));
    task->kind = DLD_TASK_DOWNLOAD;
    task->collision = DLD_COLLISION_RENAME;
    task->status = DLD_STATUS_QUEUED;
    dld_app_error_init(&task->error);
}

void dld_task_record_clear(DldTaskRecord *task)
{
    if (task == NULL) {
        return;
    }

    free(task->id);
    free(task->input_url);
    free(task->input_path);
    free(task->options_json);
    free(task->destination);
    free(task->temporary_path);
    dld_app_error_clear(&task->error);
    dld_task_record_init(task);
}

bool dld_task_record_copy(DldTaskRecord *destination, const DldTaskRecord *source)
{
    if (destination == NULL || source == NULL || destination == source) {
        return false;
    }

    DldTaskRecord copy;
    dld_task_record_init(&copy);

    copy.id = dld_string_duplicate(source->id);
    copy.input_url = dld_string_duplicate(source->input_url);
    copy.input_path = dld_string_duplicate(source->input_path);
    copy.options_json = dld_string_duplicate(source->options_json);
    copy.destination = dld_string_duplicate(source->destination);
    copy.temporary_path = dld_string_duplicate(source->temporary_path);

    const bool string_copy_failed =
        (source->id != NULL && copy.id == NULL) ||
        (source->input_url != NULL && copy.input_url == NULL) ||
        (source->input_path != NULL && copy.input_path == NULL) ||
        (source->options_json != NULL && copy.options_json == NULL) ||
        (source->destination != NULL && copy.destination == NULL) ||
        (source->temporary_path != NULL && copy.temporary_path == NULL);

    if (string_copy_failed) {
        dld_task_record_clear(&copy);
        return false;
    }

    copy.kind = source->kind;
    copy.collision = source->collision;
    copy.status = source->status;
    copy.has_error = source->has_error;
    copy.created_at_ms = source->created_at_ms;
    copy.updated_at_ms = source->updated_at_ms;

    if (source->has_error && !dld_app_error_copy(&copy.error, &source->error)) {
        dld_task_record_clear(&copy);
        return false;
    }

    dld_task_record_clear(destination);
    *destination = copy;
    return true;
}
