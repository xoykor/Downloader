#include "downloader/process.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool saw_stderr;
} LineCapture;

static void capture_line(const char *line, void *userdata)
{
    LineCapture *capture = userdata;
    if (line != NULL && strstr(line, "definitely-missing") != NULL) {
        capture->saw_stderr = true;
    }
}

int main(void)
{
    DldAppError error;
    dld_app_error_init(&error);

    DldProcessResult result;
    dld_process_result_init(&result);

    char *printf_argv[] = {"printf", "ok", NULL};
    DldProcessSpec printf_spec = {
        .program = "printf",
        .argv = printf_argv,
        .timeout_ms = 1000U,
    };
    assert(dld_process_run(
        &printf_spec,
        NULL,
        NULL,
        NULL,
        &result,
        &error));
    assert(result.exit_code == 0);
    assert(strcmp(result.stdout_text, "ok") == 0);
    dld_process_result_clear(&result);

    /*
     * cat escreve a mensagem de arquivo ausente em stderr. O callback combinado
     * deve recebê-la, pois yt-dlp também usa stderr para progresso em runtime.
     */
    LineCapture capture = {0};
    char *cat_argv[] = {
        "cat",
        "/tmp/downloader-definitely-missing",
        NULL,
    };
    DldProcessSpec cat_spec = {
        .program = "cat",
        .argv = cat_argv,
        .timeout_ms = 1000U,
    };
    assert(dld_process_run(
        &cat_spec,
        NULL,
        capture_line,
        &capture,
        &result,
        &error));
    assert(result.exit_code != 0);
    assert(capture.saw_stderr);
    dld_process_result_clear(&result);

    char *sleep_argv[] = {"sleep", "2", NULL};
    DldProcessSpec sleep_spec = {
        .program = "sleep",
        .argv = sleep_argv,
        .timeout_ms = 20U,
    };
    assert(dld_process_run(
        &sleep_spec,
        NULL,
        NULL,
        NULL,
        &result,
        &error));
    assert(result.timed_out);

    dld_process_result_clear(&result);
    dld_app_error_clear(&error);

    puts("test_process: ok");
    return 0;
}
