/*
 * Índice local usado para reconhecer downloads já concluídos.
 *
 * O arquivo de índice é apenas uma dica de identidade. Antes de devolver um
 * caminho, o código reconstrói o caminho dentro da pasta de destino e confirma
 * que o arquivo ainda existe, evitando confiar cegamente em JSON antigo.
 */

#include "downloader/library.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *index_path(const char *directory)
{
    const size_t length = strlen(directory) + strlen("/.downloader-library.json") + 1U;
    char *path = malloc(length);
    if (path != NULL) (void)snprintf(path, length, "%s/.downloader-library.json", directory);
    return path;
}

static bool regular_file(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static struct json_object *load_index(const char *path)
{
    struct json_object *root = json_object_from_file(path);
    if (root == NULL || !json_object_is_type(root, json_type_object)) {
        if (root != NULL) json_object_put(root);
        root = json_object_new_object();
    }
    return root;
}

static char *entry_key(const char *media_id, const char *format)
{
    const size_t length = strlen(media_id) + strlen(format) + 2U;
    char *key = malloc(length);
    if (key != NULL) (void)snprintf(key, length, "%s|%s", media_id, format);
    return key;
}

bool dld_library_find(const char *directory, const char *media_id, const char *format,
                      char **path, DldAppError *error)
{
    (void)error;
    if (path != NULL) *path = NULL;
    if (directory == NULL || media_id == NULL || format == NULL || path == NULL) return false;
    char *file = index_path(directory);
    char *key = entry_key(media_id, format);
    if (file == NULL || key == NULL) {
        free(file); free(key);
        return false;
    }
    struct json_object *root = load_index(file);
    struct json_object *entry = NULL;
    bool found = false;
    if (root != NULL && json_object_object_get_ex(root, key, &entry) &&
        json_object_is_type(entry, json_type_string)) {
        const char *name = json_object_get_string(entry);
        /* O índice grava somente basename; isso impede escapar da pasta destino. */
        if (name != NULL && strchr(name, '/') == NULL) {
            const size_t length = strlen(directory) + strlen(name) + 2U;
            char *candidate = malloc(length);
            if (candidate != NULL) {
                (void)snprintf(candidate, length, "%s/%s", directory, name);
                if (regular_file(candidate)) {
                    *path = candidate;
                    found = true;
                } else {
                    free(candidate);
                }
            }
        }
    }
    if (root != NULL) json_object_put(root);
    free(file); free(key);
    return found;
}

bool dld_library_record(const char *directory, const char *media_id, const char *format,
                        const char *published_path, DldAppError *error)
{
    if (directory == NULL || media_id == NULL || format == NULL || published_path == NULL) return false;
    const char *basename = strrchr(published_path, '/');
    basename = basename == NULL ? published_path : basename + 1;
    if (*basename == '\0') return false;

    char *file = index_path(directory);
    char *key = entry_key(media_id, format);
    if (file == NULL || key == NULL) {
        free(file); free(key);
        return false;
    }
    struct json_object *root = load_index(file);
    if (root == NULL) {
        free(file); free(key);
        return false;
    }
    json_object_object_add(root, key, json_object_new_string(basename));

    const size_t temp_len = strlen(file) + 5U;
    char *temporary = malloc(temp_len);
    if (temporary == NULL) {
        json_object_put(root); free(file); free(key);
        return false;
    }
    (void)snprintf(temporary, temp_len, "%s.tmp", file);
    const char *serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    FILE *out = fopen(temporary, "wb");
    bool ok = out != NULL;
    if (ok) {
        const size_t length = strlen(serialized);
        ok = fwrite(serialized, 1U, length, out) == length && fflush(out) == 0;
        if (fclose(out) != 0) ok = false;
    }
    if (ok && rename(temporary, file) != 0) ok = false;
    if (!ok) {
        (void)unlink(temporary);
        (void)dld_app_error_set(error, DLD_ERROR_PERMISSION,
                                "Não foi possível atualizar o índice de downloads.",
                                "biblioteca", true, errno);
    }
    free(temporary);
    json_object_put(root);
    free(file); free(key);
    return ok;
}
