#include "downloader/process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

static void buffer_init(TextBuffer *buffer)
{
    buffer->data = NULL;
    buffer->length = 0U;
    buffer->capacity = 0U;
}

static void buffer_clear(TextBuffer *buffer)
{
    free(buffer->data);
    buffer_init(buffer);
}

static bool buffer_reserve(TextBuffer *buffer, size_t required)
{
    if (required <= buffer->capacity) return true;
    size_t capacity = buffer->capacity == 0U ? 4096U : buffer->capacity;
    while (capacity < required) capacity *= 2U;
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) return false;
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static bool buffer_append(TextBuffer *buffer, const char *data, size_t length)
{
    if (!buffer_reserve(buffer, buffer->length + length + 1U)) return false;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static char *buffer_take(TextBuffer *buffer)
{
    if (buffer->data == NULL) return dld_string_duplicate("");
    char *value = buffer->data;
    buffer_init(buffer);
    return value;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return ((uint64_t)now.tv_sec * UINT64_C(1000)) +
           ((uint64_t)now.tv_nsec / UINT64_C(1000000));
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void emit_complete_lines(TextBuffer *pending, DldProcessLineCallback callback,
                                void *userdata)
{
    if (callback == NULL || pending->data == NULL) return;
    size_t start = 0U;
    for (size_t i = 0U; i < pending->length; ++i) {
        if (pending->data[i] != '\n') continue;
        pending->data[i] = '\0';
        callback(pending->data + start, userdata);
        start = i + 1U;
    }
    if (start > 0U) {
        const size_t remaining = pending->length - start;
        memmove(pending->data, pending->data + start, remaining);
        pending->length = remaining;
        pending->data[remaining] = '\0';
    }
}

static bool read_pipe(int fd, TextBuffer *whole, TextBuffer *line_buffer,
                      DldProcessLineCallback callback, void *userdata,
                      bool *reached_eof)
{
    char chunk[4096];
    for (;;) {
        const ssize_t count = read(fd, chunk, sizeof(chunk));
        if (count > 0) {
            const size_t size = (size_t)count;
            if (!buffer_append(whole, chunk, size)) return false;
            if (callback != NULL) {
                if (!buffer_append(line_buffer, chunk, size)) return false;
                emit_complete_lines(line_buffer, callback, userdata);
            }
            continue;
        }
        if (count == 0) {
            *reached_eof = true;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

void dld_process_result_init(DldProcessResult *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
}

void dld_process_result_clear(DldProcessResult *result)
{
    if (result == NULL) return;
    free(result->stdout_text);
    free(result->stderr_text);
    dld_process_result_init(result);
}

bool dld_process_run(const DldProcessSpec *spec, atomic_bool *cancel_flag,
                     DldProcessLineCallback on_stdout_line, void *userdata,
                     DldProcessResult *result, DldAppError *error)
{
    if (spec == NULL || spec->program == NULL || spec->argv == NULL || result == NULL) {
        (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Especificação de processo inválida.",
                                "processo", false, 0);
        return false;
    }

    dld_process_result_clear(result);
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Não foi possível criar pipes.",
                                "processo", true, errno);
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        const int saved = errno;
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        (void)dld_app_error_set(error, DLD_ERROR_DEPENDENCY, "Não foi possível iniciar processo.",
                                "processo", true, saved);
        return false;
    }

    if (pid == 0) {
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)dup2(err_pipe[1], STDERR_FILENO);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        if (spec->working_directory != NULL && chdir(spec->working_directory) != 0) _exit(126);
        execvp(spec->program, spec->argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[1]);
    (void)set_nonblocking(out_pipe[0]);
    (void)set_nonblocking(err_pipe[0]);

    TextBuffer stdout_buffer, stderr_buffer, line_buffer;
    buffer_init(&stdout_buffer);
    buffer_init(&stderr_buffer);
    buffer_init(&line_buffer);
    bool stdout_eof = false;
    bool stderr_eof = false;
    bool child_exited = false;
    int child_status = 0;
    const uint64_t started = monotonic_ms();
    bool sent_termination = false;
    uint64_t termination_sent_at = 0U;

    while (!child_exited || !stdout_eof || !stderr_eof) {
        if (!sent_termination && cancel_flag != NULL && atomic_load(cancel_flag)) {
            result->cancelled = true;
            (void)kill(pid, SIGTERM);
            sent_termination = true;
            termination_sent_at = monotonic_ms();
        }
        if (!sent_termination && spec->timeout_ms > 0U) {
            const uint64_t elapsed = monotonic_ms() - started;
            if (elapsed >= (uint64_t)spec->timeout_ms) {
                result->timed_out = true;
                (void)kill(pid, SIGTERM);
                sent_termination = true;
                termination_sent_at = monotonic_ms();
            }
        }

        if (sent_termination && !child_exited && termination_sent_at > 0U &&
            monotonic_ms() - termination_sent_at >= UINT64_C(2000)) {
            (void)kill(pid, SIGKILL);
            termination_sent_at = 0U;
        }

        struct pollfd fds[2] = {
            {.fd = stdout_eof ? -1 : out_pipe[0], .events = POLLIN | POLLHUP},
            {.fd = stderr_eof ? -1 : err_pipe[0], .events = POLLIN | POLLHUP},
        };
        (void)poll(fds, 2, 50);

        if (!stdout_eof && (fds[0].revents & (POLLIN | POLLHUP)) != 0) {
            if (!read_pipe(out_pipe[0], &stdout_buffer, &line_buffer, on_stdout_line,
                           userdata, &stdout_eof)) goto io_failure;
        }
        if (!stderr_eof && (fds[1].revents & (POLLIN | POLLHUP)) != 0) {
            TextBuffer unused_lines;
            buffer_init(&unused_lines);
            if (!read_pipe(err_pipe[0], &stderr_buffer, &unused_lines, NULL, NULL, &stderr_eof)) {
                buffer_clear(&unused_lines);
                goto io_failure;
            }
            buffer_clear(&unused_lines);
        }

        if (!child_exited) {
            const pid_t waited = waitpid(pid, &child_status, WNOHANG);
            if (waited == pid) child_exited = true;
            else if (waited < 0 && errno != EINTR) goto io_failure;
        }
    }

    if (on_stdout_line != NULL && line_buffer.length > 0U) {
        on_stdout_line(line_buffer.data, userdata);
    }
    result->exited = child_exited;
    if (child_exited && WIFEXITED(child_status)) result->exit_code = WEXITSTATUS(child_status);
    else if (child_exited && WIFSIGNALED(child_status)) result->exit_code = 128 + WTERMSIG(child_status);
    result->stdout_text = buffer_take(&stdout_buffer);
    result->stderr_text = buffer_take(&stderr_buffer);
    buffer_clear(&line_buffer);
    close_fd(&out_pipe[0]);
    close_fd(&err_pipe[0]);
    return result->stdout_text != NULL && result->stderr_text != NULL;

io_failure:
    {
        const int saved = errno;
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close_fd(&out_pipe[0]); close_fd(&err_pipe[0]);
        buffer_clear(&stdout_buffer); buffer_clear(&stderr_buffer); buffer_clear(&line_buffer);
        (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Falha ao ler saída do processo.",
                                "processo", true, saved);
        return false;
    }
}

static char *first_nonempty_line(const char *text)
{
    if (text == NULL) return NULL;
    const char *start = text;
    while (*start != '\0') {
        const char *end = strchr(start, '\n');
        const size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
        if (length > 0U) {
            char *line = malloc(length + 1U);
            if (line == NULL) return NULL;
            memcpy(line, start, length);
            line[length] = '\0';
            return line;
        }
        if (end == NULL) break;
        start = end + 1;
    }
    return NULL;
}

bool dld_process_detect(const char *program, char **resolved_path, char **version_line)
{
    if (resolved_path != NULL) *resolved_path = NULL;
    if (version_line != NULL) *version_line = NULL;
    if (program == NULL || *program == '\0') return false;

    char *path_value = dld_string_duplicate(getenv("PATH"));
    if (path_value == NULL) return false;
    char *save = NULL;
    char *found = NULL;
    for (char *dir = strtok_r(path_value, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
        const size_t length = strlen(dir) + strlen(program) + 2U;
        char *candidate = malloc(length);
        if (candidate == NULL) break;
        (void)snprintf(candidate, length, "%s/%s", dir, program);
        if (access(candidate, X_OK) == 0) {
            found = candidate;
            break;
        }
        free(candidate);
    }
    free(path_value);
    if (found == NULL) return false;

    if (resolved_path != NULL) *resolved_path = dld_string_duplicate(found);
    if (version_line != NULL) {
        char *argv[] = {found, "--version", NULL};
        DldProcessSpec spec = {.program = found, .argv = argv, .timeout_ms = 3000U};
        DldProcessResult result;
        DldAppError error;
        dld_process_result_init(&result);
        dld_app_error_init(&error);
        if (dld_process_run(&spec, NULL, NULL, NULL, &result, &error)) {
            *version_line = first_nonempty_line(result.stdout_text);
            if (*version_line == NULL) *version_line = first_nonempty_line(result.stderr_text);
        }
        dld_process_result_clear(&result);
        dld_app_error_clear(&error);
    }
    free(found);
    return resolved_path == NULL || *resolved_path != NULL;
}
