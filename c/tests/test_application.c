#include "downloader/application.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *owned_string(const char *text)
{
    const size_t size = strlen(text) + 1U;
    char *copy = malloc(size);
    assert(copy != NULL);
    memcpy(copy, text, size);
    return copy;
}

static DldTaskRecord make_task(const char *id, DldTaskKind kind)
{
    DldTaskRecord task;
    dld_task_record_init(&task);
    task.id = owned_string(id);
    task.kind = kind;
    task.options_json = owned_string("{}");
    return task;
}

static void test_conversion_limit(void)
{
    DldApplicationState state;
    DldQueueLimits limits = {
        .max_downloads = 2U,
        .max_conversions = 1U,
    };
    dld_application_state_init(&state, limits);

    DldAppError error;
    dld_app_error_init(&error);
    DldTaskEvent event;

    DldTaskRecord first = make_task("a", DLD_TASK_CONVERT);
    DldTaskRecord second = make_task("b", DLD_TASK_CONVERT);

    assert(dld_application_enqueue(&state, &first, &event, &error));
    assert(dld_application_enqueue(&state, &second, &event, &error));

    assert(dld_application_start_next(&state, &event, &error) == DLD_START_STARTED);
    assert(strcmp(event.task_id, "a") == 0);
    assert(dld_application_start_next(&state, &event, &error) == DLD_START_NONE);

    assert(dld_application_finish(
        &state,
        "a",
        DLD_STATUS_COMPLETED,
        NULL,
        &event,
        &error
    ));

    assert(dld_application_start_next(&state, &event, &error) == DLD_START_STARTED);
    assert(strcmp(event.task_id, "b") == 0);

    dld_task_record_clear(&first);
    dld_task_record_clear(&second);
    dld_application_state_clear(&state);
    dld_app_error_clear(&error);
}

static void test_recovery_marks_active_task_interrupted(void)
{
    DldApplicationState state;
    dld_application_state_init(&state, dld_queue_limits_default());

    DldAppError error;
    dld_app_error_init(&error);

    DldTaskRecord task = make_task("recover-me", DLD_TASK_DOWNLOAD);
    task.status = DLD_STATUS_DOWNLOADING;

    assert(dld_application_restore(&state, &task, &error));
    assert(dld_application_recover_after_restart(&state) == 1U);

    const DldTaskRecord *stored = dld_application_find_task(&state, "recover-me");
    assert(stored != NULL);
    assert(stored->status == DLD_STATUS_INTERRUPTED);

    dld_task_record_clear(&task);
    dld_application_state_clear(&state);
    dld_app_error_clear(&error);
}

static void test_retry_only_accepts_retryable_states(void)
{
    DldApplicationState state;
    dld_application_state_init(&state, dld_queue_limits_default());

    DldAppError error;
    dld_app_error_init(&error);
    DldTaskEvent event;

    DldTaskRecord task = make_task("retry", DLD_TASK_DOWNLOAD);
    assert(dld_application_enqueue(&state, &task, &event, &error));
    assert(!dld_application_retry(&state, "retry", &event, &error));

    dld_app_error_clear(&error);
    assert(dld_application_cancel(&state, "retry", &event, &error));
    assert(dld_application_retry(&state, "retry", &event, &error));

    const DldTaskRecord *stored = dld_application_find_task(&state, "retry");
    assert(stored != NULL);
    assert(stored->status == DLD_STATUS_QUEUED);

    dld_task_record_clear(&task);
    dld_application_state_clear(&state);
    dld_app_error_clear(&error);
}

int main(void)
{
    test_conversion_limit();
    test_recovery_marks_active_task_interrupted();
    test_retry_only_accepts_retryable_states();
    puts("test_application: ok");
    return 0;
}
