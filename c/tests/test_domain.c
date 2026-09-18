#include "downloader/domain.h"

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

static void test_status_rules(void)
{
    assert(dld_task_status_is_active(DLD_STATUS_DOWNLOADING));
    assert(!dld_task_status_is_active(DLD_STATUS_INTERRUPTED));
    assert(dld_task_status_is_terminal(DLD_STATUS_COMPLETED));
    assert(!dld_task_status_is_terminal(DLD_STATUS_QUEUED));
}

static void test_deep_copy(void)
{
    DldTaskRecord source;
    DldTaskRecord copy;
    dld_task_record_init(&source);
    dld_task_record_init(&copy);

    source.id = owned_string("task-1");
    source.input_path = owned_string("/tmp/vídeo.mkv");
    source.options_json = owned_string("{\"format\":\"mp4\"}");
    source.kind = DLD_TASK_CONVERT;
    source.status = DLD_STATUS_FAILED;
    source.has_error = true;

    assert(dld_app_error_set(
        &source.error,
        DLD_ERROR_INVALID_MEDIA,
        "arquivo inválido",
        "validação",
        false,
        0
    ));

    assert(dld_task_record_copy(&copy, &source));
    assert(copy.id != source.id);
    assert(strcmp(copy.id, source.id) == 0);
    assert(copy.input_path != source.input_path);
    assert(strcmp(copy.options_json, source.options_json) == 0);
    assert(copy.has_error);
    assert(strcmp(copy.error.message, "arquivo inválido") == 0);

    dld_task_record_clear(&source);

    /* A cópia deve continuar válida depois que a origem for destruída. */
    assert(strcmp(copy.id, "task-1") == 0);
    assert(strcmp(copy.input_path, "/tmp/vídeo.mkv") == 0);
    dld_task_record_clear(&copy);
}

int main(void)
{
    test_status_rules();
    test_deep_copy();
    puts("test_domain: ok");
    return 0;
}
