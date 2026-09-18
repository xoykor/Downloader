#include "downloader/domain.h"

#include <stdlib.h>
#include <string.h>

char *dld_string_duplicate(const char *text)
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
    return status == DLD_STATUS_COMPLETED || status == DLD_STATUS_FAILED ||
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
    static const char *names[] = {"download", "merge", "convert", "validate"};
    return (unsigned)kind < 4U ? names[(unsigned)kind] : "unknown";
}

bool dld_task_kind_from_name(const char *name, DldTaskKind *kind)
{
    if (name == NULL || kind == NULL) return false;
    for (unsigned i = 0; i < 4U; ++i) {
        if (strcmp(name, dld_task_kind_name((DldTaskKind)i)) == 0) {
            *kind = (DldTaskKind)i;
            return true;
        }
    }
    return false;
}

const char *dld_task_status_name(DldTaskStatus status)
{
    static const char *names[] = {
        "na_fila", "analisando", "aguardando_autenticacao", "baixando",
        "unindo", "convertendo", "validando", "concluido", "falhou",
        "cancelado", "interrompido"
    };
    return (unsigned)status < 11U ? names[(unsigned)status] : "desconhecido";
}

bool dld_task_status_from_name(const char *name, DldTaskStatus *status)
{
    if (name == NULL || status == NULL) return false;
    for (unsigned i = 0; i < 11U; ++i) {
        if (strcmp(name, dld_task_status_name((DldTaskStatus)i)) == 0) {
            *status = (DldTaskStatus)i;
            return true;
        }
    }
    return false;
}

const char *dld_collision_policy_name(DldCollisionPolicy policy)
{
    static const char *names[] = {"rename", "replace", "skip"};
    return (unsigned)policy < 3U ? names[(unsigned)policy] : "rename";
}

bool dld_collision_policy_from_name(const char *name, DldCollisionPolicy *policy)
{
    if (name == NULL || policy == NULL) return false;
    for (unsigned i = 0; i < 3U; ++i) {
        if (strcmp(name, dld_collision_policy_name((DldCollisionPolicy)i)) == 0) {
            *policy = (DldCollisionPolicy)i;
            return true;
        }
    }
    return false;
}

const char *dld_auth_kind_name(DldAuthKind kind)
{
    static const char *names[] = {"none", "cookie_file", "browser_profile"};
    return (unsigned)kind < 3U ? names[(unsigned)kind] : "none";
}

bool dld_auth_kind_from_name(const char *name, DldAuthKind *kind)
{
    if (name == NULL || kind == NULL) return false;
    for (unsigned i = 0; i < 3U; ++i) {
        if (strcmp(name, dld_auth_kind_name((DldAuthKind)i)) == 0) {
            *kind = (DldAuthKind)i;
            return true;
        }
    }
    return false;
}

void dld_auth_ref_init(DldAuthRef *auth)
{
    if (auth == NULL) return;
    auth->kind = DLD_AUTH_NONE;
    auth->id = NULL;
}

void dld_auth_ref_clear(DldAuthRef *auth)
{
    if (auth == NULL) return;
    free(auth->id);
    dld_auth_ref_init(auth);
}

bool dld_auth_ref_set(DldAuthRef *auth, DldAuthKind kind, const char *id)
{
    if (auth == NULL) return false;
    char *copy = dld_string_duplicate(id);
    if (id != NULL && copy == NULL) return false;
    dld_auth_ref_clear(auth);
    auth->kind = kind;
    auth->id = copy;
    return true;
}

bool dld_auth_ref_copy(DldAuthRef *destination, const DldAuthRef *source)
{
    if (destination == NULL || source == NULL) return false;
    return dld_auth_ref_set(destination, source->kind, source->id);
}

void dld_app_error_init(DldAppError *error)
{
    if (error == NULL) return;
    error->category = DLD_ERROR_INTERNAL;
    error->message = NULL;
    error->step = NULL;
    error->has_code = false;
    error->code = 0;
}

void dld_app_error_clear(DldAppError *error)
{
    if (error == NULL) return;
    free(error->message);
    free(error->step);
    dld_app_error_init(error);
}

bool dld_app_error_set(DldAppError *error, DldErrorCategory category,
                       const char *message, const char *step,
                       bool has_code, int code)
{
    if (error == NULL) return false;
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
    if (destination == NULL || source == NULL) return false;
    return dld_app_error_set(destination, source->category, source->message,
                             source->step, source->has_code, source->code);
}

void dld_task_record_init(DldTaskRecord *task)
{
    if (task == NULL) return;
    memset(task, 0, sizeof(*task));
    task->kind = DLD_TASK_DOWNLOAD;
    task->collision = DLD_COLLISION_RENAME;
    task->status = DLD_STATUS_QUEUED;
    dld_auth_ref_init(&task->auth);
    dld_app_error_init(&task->error);
}

void dld_task_record_clear(DldTaskRecord *task)
{
    if (task == NULL) return;
    free(task->id);
    free(task->input_url);
    free(task->input_path);
    free(task->options_json);
    free(task->destination);
    free(task->temporary_path);
    dld_auth_ref_clear(&task->auth);
    dld_app_error_clear(&task->error);
    dld_task_record_init(task);
}

bool dld_task_record_copy(DldTaskRecord *destination, const DldTaskRecord *source)
{
    if (destination == NULL || source == NULL || destination == source) return false;

    DldTaskRecord copy;
    dld_task_record_init(&copy);
    copy.id = dld_string_duplicate(source->id);
    copy.input_url = dld_string_duplicate(source->input_url);
    copy.input_path = dld_string_duplicate(source->input_path);
    copy.options_json = dld_string_duplicate(source->options_json);
    copy.destination = dld_string_duplicate(source->destination);
    copy.temporary_path = dld_string_duplicate(source->temporary_path);

    const bool failed =
        (source->id != NULL && copy.id == NULL) ||
        (source->input_url != NULL && copy.input_url == NULL) ||
        (source->input_path != NULL && copy.input_path == NULL) ||
        (source->options_json != NULL && copy.options_json == NULL) ||
        (source->destination != NULL && copy.destination == NULL) ||
        (source->temporary_path != NULL && copy.temporary_path == NULL);
    if (failed) {
        dld_task_record_clear(&copy);
        return false;
    }

    copy.kind = source->kind;
    copy.collision = source->collision;
    copy.status = source->status;
    copy.has_auth = source->has_auth;
    copy.has_error = source->has_error;
    copy.created_at_ms = source->created_at_ms;
    copy.updated_at_ms = source->updated_at_ms;

    if (source->has_auth && !dld_auth_ref_copy(&copy.auth, &source->auth)) {
        dld_task_record_clear(&copy);
        return false;
    }
    if (source->has_error && !dld_app_error_copy(&copy.error, &source->error)) {
        dld_task_record_clear(&copy);
        return false;
    }

    dld_task_record_clear(destination);
    *destination = copy;
    return true;
}
