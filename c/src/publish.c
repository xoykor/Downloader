/*
 * Publicação do temporário no destino final.
 *
 * A engine chama este módulo somente depois da validação por ffprobe. Quando
 * `rename` cruza filesystems (EXDEV), a cópia é sincronizada antes de remover o
 * temporário, preservando a entrada original se a gravação falhar no meio.
 */

#include "downloader/publish.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void dld_published_output_init(DldPublishedOutput *output)
{
    if (output == NULL) return;
    output->path = NULL;
    output->skipped = false;
}

void dld_published_output_clear(DldPublishedOutput *output)
{
    if (output == NULL) return;
    free(output->path);
    dld_published_output_init(output);
}

static bool path_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

static bool is_regular_file(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool ensure_directory(const char *directory, DldAppError *error)
{
    if (directory == NULL || *directory == '\0' || strcmp(directory, ".") == 0) return true;
    char *copy = dld_string_duplicate(directory);
    if (copy == NULL) return false;
    for (char *p = copy + 1; *p != '\0'; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            (void)dld_app_error_set(error, DLD_ERROR_PERMISSION,
                                    "Não foi possível criar diretório de saída.",
                                    "publicação", true, errno);
            free(copy);
            return false;
        }
        *p = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        (void)dld_app_error_set(error, DLD_ERROR_PERMISSION,
                                "Não foi possível criar diretório de saída.",
                                "publicação", true, errno);
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static char *parent_directory(const char *path)
{
    char *copy = dld_string_duplicate(path);
    if (copy == NULL) return NULL;
    char *slash = strrchr(copy, '/');
    if (slash == NULL) {
        free(copy);
        return dld_string_duplicate(".");
    }
    if (slash == copy) slash[1] = '\0';
    else *slash = '\0';
    return copy;
}

static bool copy_and_sync(const char *source_path, const char *destination_path,
                          DldAppError *error)
{
    const int source = open(source_path, O_RDONLY);
    if (source < 0) goto fail;
    const int destination = open(destination_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (destination < 0) {
        close(source);
        goto fail;
    }
    char buffer[65536];
    bool ok = true;
    for (;;) {
        const ssize_t read_count = read(source, buffer, sizeof(buffer));
        if (read_count == 0) break;
        if (read_count < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        ssize_t written = 0;
        while (written < read_count) {
            const ssize_t count = write(destination, buffer + written, (size_t)(read_count - written));
            if (count < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            written += count;
        }
        if (!ok) break;
    }
    if (ok && fsync(destination) != 0) ok = false;
    const int saved = errno;
    close(source);
    close(destination);
    if (!ok) {
        errno = saved;
        goto fail;
    }
    if (unlink(source_path) != 0) goto fail;
    return true;
fail:
    (void)dld_app_error_set(error, DLD_ERROR_PERMISSION,
                            "Falha ao copiar arquivo para o destino.",
                            "publicação", true, errno);
    return false;
}

static bool move_safely(const char *source, const char *destination, DldAppError *error)
{
    if (rename(source, destination) == 0) return true;
    if (errno == EXDEV) return copy_and_sync(source, destination, error);
    (void)dld_app_error_set(error, DLD_ERROR_PERMISSION,
                            "Falha ao publicar arquivo final.", "publicação", true, errno);
    return false;
}

static char *renamed_candidate(const char *destination)
{
    const char *slash = strrchr(destination, '/');
    const char *name = slash == NULL ? destination : slash + 1;
    const char *dot = strrchr(name, '.');
    const size_t prefix_len = dot == NULL ? strlen(destination) :
                              (size_t)((slash == NULL ? destination : slash + 1) - destination) +
                              (size_t)(dot - name);
    const char *extension = dot == NULL ? "" : dot;
    for (unsigned n = 1U; n < 1000000U; ++n) {
        const size_t needed = prefix_len + strlen(extension) + 32U;
        char *candidate = malloc(needed);
        if (candidate == NULL) return NULL;
        (void)snprintf(candidate, needed, "%.*s (%u)%s", (int)prefix_len, destination, n, extension);
        if (!path_exists(candidate)) return candidate;
        free(candidate);
    }
    return NULL;
}

/*
 * Esta é a fronteira entre "resultado em construção" e "arquivo do usuário".
 * A função nunca publica um temporário inexistente e resolve colisões antes do
 * movimento final, para que política de nome não fique espalhada pela engine.
 */
bool dld_publish_output(const char *temporary, const char *destination,
                        DldCollisionPolicy policy, DldPublishedOutput *output,
                        DldAppError *error)
{
    if (output == NULL || !is_regular_file(temporary) || destination == NULL) {
        (void)dld_app_error_set(error, DLD_ERROR_INVALID_MEDIA,
                                "Arquivo temporário não encontrado.", "publicação", false, 0);
        return false;
    }
    dld_published_output_clear(output);
    char *parent = parent_directory(destination);
    if (parent == NULL || !ensure_directory(parent, error)) {
        free(parent);
        return false;
    }
    free(parent);

    char *target = dld_string_duplicate(destination);
    if (target == NULL) return false;
    if (path_exists(destination)) {
        if (policy == DLD_COLLISION_SKIP) {
            if (unlink(temporary) != 0) {
                free(target);
                return false;
            }
            output->path = target;
            output->skipped = true;
            return true;
        }
        if (policy == DLD_COLLISION_RENAME) {
            free(target);
            target = renamed_candidate(destination);
            if (target == NULL) return false;
        }
    }
    if (!move_safely(temporary, target, error)) {
        free(target);
        return false;
    }
    output->path = target;
    output->skipped = false;
    return true;
}

bool dld_cleanup_temporary(const char *path, DldAppError *error)
{
    if (path == NULL) return true;
    if (unlink(path) == 0 || errno == ENOENT) return true;
    (void)dld_app_error_set(error, DLD_ERROR_PERMISSION,
                            "Não foi possível remover arquivo temporário.",
                            "limpeza", true, errno);
    return false;
}
