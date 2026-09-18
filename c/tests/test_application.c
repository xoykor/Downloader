#include "downloader/application.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static DldTaskRecord task(const char *id, DldTaskKind kind)
{
    DldTaskRecord value;
    dld_task_record_init(&value);
    value.id = dld_string_duplicate(id);
    value.kind = kind;
    value.options_json = dld_string_duplicate("{}");
    return value;
}

int main(void)
{
    DldApplicationState state;
    dld_application_state_init(&state, (DldQueueLimits){2U, 1U});
    DldAppError error; dld_app_error_init(&error);
    DldTaskEvent event;
    DldTaskRecord a = task("a", DLD_TASK_CONVERT);
    DldTaskRecord b = task("b", DLD_TASK_CONVERT);
    assert(dld_application_enqueue(&state, &a, &event, &error));
    assert(dld_application_enqueue(&state, &b, &event, &error));
    assert(dld_application_start_next(&state, &event, &error) == DLD_START_STARTED);
    assert(strcmp(event.task_id, "a") == 0);
    assert(dld_application_start_next(&state, &event, &error) == DLD_START_NONE);
    assert(dld_application_finish(&state, "a", DLD_STATUS_COMPLETED, NULL, &event, &error));
    assert(dld_application_start_next(&state, &event, &error) == DLD_START_STARTED);
    assert(strcmp(event.task_id, "b") == 0);
    dld_task_record_clear(&a); dld_task_record_clear(&b);
    dld_application_state_clear(&state); dld_app_error_clear(&error);
    puts("test_application: ok");
    return 0;
}
