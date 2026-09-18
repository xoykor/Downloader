#include "downloader/domain.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_status_rules(void)
{
    assert(dld_task_status_is_active(DLD_STATUS_DOWNLOADING));
    assert(!dld_task_status_is_active(DLD_STATUS_INTERRUPTED));
    assert(dld_task_status_is_terminal(DLD_STATUS_COMPLETED));
    assert(!dld_task_status_is_terminal(DLD_STATUS_QUEUED));
}

static void test_deep_copy(void)
{
    DldTaskRecord source, copy;
    dld_task_record_init(&source);
    dld_task_record_init(&copy);
    source.id = dld_string_duplicate("task-1");
    source.input_path = dld_string_duplicate("/tmp/in vídeo.mkv");
    source.options_json = dld_string_duplicate("{\"format\":\"mp4\"}");
    source.kind = DLD_TASK_CONVERT;
    source.has_auth = true;
    assert(dld_auth_ref_set(&source.auth, DLD_AUTH_BROWSER_PROFILE, "firefox:default"));
    source.has_error = true;
    assert(dld_app_error_set(&source.error, DLD_ERROR_INVALID_MEDIA,
                             "inválido", "validação", false, 0));
    assert(dld_task_record_copy(&copy, &source));
    dld_task_record_clear(&source);
    assert(strcmp(copy.id, "task-1") == 0);
    assert(strcmp(copy.auth.id, "firefox:default") == 0);
    assert(strcmp(copy.error.message, "inválido") == 0);
    dld_task_record_clear(&copy);
}

int main(void)
{
    test_status_rules();
    test_deep_copy();
    puts("test_domain: ok");
    return 0;
}
