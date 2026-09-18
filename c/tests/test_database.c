#include "downloader/database.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/downloader-db-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
    DldDatabase db; dld_database_init(&db);
    DldAppError error; dld_app_error_init(&error);
    assert(dld_database_open(&db, path, &error));
    DldTaskRecord task; dld_task_record_init(&task);
    task.id = dld_string_duplicate("1"); task.kind = DLD_TASK_DOWNLOAD;
    task.input_url = dld_string_duplicate("https://example.test/x");
    task.options_json = dld_string_duplicate("{\"format\":\"mp4\"}");
    task.status = DLD_STATUS_COMPLETED; task.created_at_ms = 1U; task.updated_at_ms = 2U;
    task.has_auth = true; assert(dld_auth_ref_set(&task.auth, DLD_AUTH_BROWSER_PROFILE, "firefox"));
    assert(dld_database_put_task(&db, &task, &error));
    DldTaskRecord loaded; dld_task_record_init(&loaded); bool found = false;
    assert(dld_database_get_task(&db, "1", &loaded, &found, &error)); assert(found);
    assert(strcmp(loaded.input_url, task.input_url) == 0);
    assert(loaded.has_auth && strcmp(loaded.auth.id, "firefox") == 0);
    assert(dld_database_record_youtube_start(&db, 1000U, &error));
    unsigned count = 0U; uint64_t oldest = 0U;
    assert(dld_database_count_youtube_starts_since(&db, 0U, &count, &oldest, &error));
    assert(count == 1U && oldest == 1000U);
    dld_task_record_clear(&loaded); dld_task_record_clear(&task);
    dld_database_close(&db); dld_app_error_clear(&error); unlink(path);
    puts("test_database: ok");
    return 0;
}
