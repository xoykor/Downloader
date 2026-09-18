#ifndef DOWNLOADER_DATABASE_H
#define DOWNLOADER_DATABASE_H

#include "downloader/domain.h"

#include <stddef.h>
#include <stdint.h>

struct sqlite3;

typedef struct {
    struct sqlite3 *handle;
    char *path;
} DldDatabase;

void dld_database_init(DldDatabase *database);
void dld_database_close(DldDatabase *database);
bool dld_database_open(DldDatabase *database, const char *path, DldAppError *error);

bool dld_database_put_task(DldDatabase *database, const DldTaskRecord *task, DldAppError *error);
bool dld_database_get_task(DldDatabase *database, const char *id,
                           DldTaskRecord *task, bool *found, DldAppError *error);
bool dld_database_list_tasks(DldDatabase *database, DldTaskRecord **tasks,
                             size_t *count, DldAppError *error);
void dld_database_free_task_list(DldTaskRecord *tasks, size_t count);

/* Persistência do limitador conservador: 300 inícios em janela móvel de 90 min. */
bool dld_database_record_youtube_start(DldDatabase *database, uint64_t timestamp_ms,
                                       DldAppError *error);
bool dld_database_count_youtube_starts_since(DldDatabase *database, uint64_t since_ms,
                                             unsigned *count, uint64_t *oldest_ms,
                                             DldAppError *error);
bool dld_database_prune_youtube_starts(DldDatabase *database, uint64_t before_ms,
                                       DldAppError *error);

#endif
