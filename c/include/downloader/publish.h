#ifndef DOWNLOADER_PUBLISH_H
#define DOWNLOADER_PUBLISH_H

/*
 * Publicação segura de arquivos temporários.
 *
 * A publicação é o único ponto que torna um resultado visível no destino final.
 * `DldPublishedOutput.path` pertence à struct e é liberado por `*_clear`.
 */

#include "downloader/domain.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *path;
    bool skipped;
} DldPublishedOutput;

void dld_published_output_init(DldPublishedOutput *output);
void dld_published_output_clear(DldPublishedOutput *output);

bool dld_publish_output(const char *temporary, const char *destination,
                        DldCollisionPolicy policy, DldPublishedOutput *output,
                        DldAppError *error);
bool dld_cleanup_temporary(const char *path, DldAppError *error);

#ifdef __cplusplus
}
#endif

#endif
