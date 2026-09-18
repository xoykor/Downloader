#ifndef DOWNLOADER_LIBRARY_H
#define DOWNLOADER_LIBRARY_H

/*
 * Índice local de downloads concluídos.
 *
 * O índice é auxiliar: um registro só pode apontar para um arquivo real dentro
 * da pasta de destino. O chamador assume ownership de `*path` quando uma busca
 * encontra resultado.
 */

#include "downloader/domain.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Índice local de downloads concluídos. Ele guarda apenas identidade, formato e
 * nome do arquivo; nunca credenciais, cookies ou URLs temporárias.
 */
bool dld_library_find(const char *directory, const char *media_id, const char *format,
                      char **path, DldAppError *error);
bool dld_library_record(const char *directory, const char *media_id, const char *format,
                        const char *published_path, DldAppError *error);

#ifdef __cplusplus
}
#endif

#endif
