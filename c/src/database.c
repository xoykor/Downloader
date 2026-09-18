#include "downloader/database.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool db_error(DldAppError *error, sqlite3 *db, const char *step, const char *fallback)
{
    const char *message = db != NULL ? sqlite3_errmsg(db) : fallback;
    return dld_app_error_set(error, DLD_ERROR_INTERNAL,
                             message != NULL ? message : fallback,
                             step, false, 0);
}

void dld_database_init(DldDatabase *database)
{
    if (database == NULL) return;
    database->handle = NULL;
    database->path = NULL;
}

void dld_database_close(DldDatabase *database)
{
    if (database == NULL) return;
    if (database->handle != NULL) (void)sqlite3_close(database->handle);
    free(database->path);
    dld_database_init(database);
}

bool dld_database_open(DldDatabase *database, const char *path, DldAppError *error)
{
    if (database == NULL || path == NULL) return false;
    dld_database_close(database);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        (void)db_error(error, db, "persistência", "Não foi possível abrir SQLite.");
        if (db != NULL) sqlite3_close(db);
        return false;
    }
    const char *schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS task_records ("
        " id TEXT PRIMARY KEY, kind TEXT NOT NULL, input_url TEXT, input_path TEXT,"
        " options_json TEXT NOT NULL, destination TEXT, collision TEXT NOT NULL,"
        " auth_kind TEXT, auth_id TEXT, status TEXT NOT NULL, temporary_path TEXT,"
        " error_category INTEGER, error_message TEXT, error_step TEXT, error_code INTEGER,"
        " created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_task_records_updated ON task_records(updated_at_ms DESC);"
        "CREATE TABLE IF NOT EXISTS youtube_starts (timestamp_ms INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS idx_youtube_starts_time ON youtube_starts(timestamp_ms);";
    char *sqlite_error = NULL;
    if (sqlite3_exec(db, schema, NULL, NULL, &sqlite_error) != SQLITE_OK) {
        (void)dld_app_error_set(error, DLD_ERROR_INTERNAL,
                                sqlite_error != NULL ? sqlite_error : "Falha ao criar banco.",
                                "persistência", false, 0);
        sqlite3_free(sqlite_error);
        sqlite3_close(db);
        return false;
    }
    database->handle = db;
    database->path = dld_string_duplicate(path);
    if (database->path == NULL) {
        dld_database_close(database);
        return false;
    }
    return true;
}

static void bind_optional_text(sqlite3_stmt *statement, int index, const char *value)
{
    if (value == NULL) sqlite3_bind_null(statement, index);
    else sqlite3_bind_text(statement, index, value, -1, SQLITE_TRANSIENT);
}

bool dld_database_put_task(DldDatabase *database, const DldTaskRecord *task, DldAppError *error)
{
    if (database == NULL || database->handle == NULL || task == NULL || task->id == NULL) return false;
    static const char *sql =
        "INSERT INTO task_records (id,kind,input_url,input_path,options_json,destination,collision,"
        "auth_kind,auth_id,status,temporary_path,error_category,error_message,error_step,error_code,"
        "created_at_ms,updated_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind,input_url=excluded.input_url,"
        "input_path=excluded.input_path,options_json=excluded.options_json,destination=excluded.destination,"
        "collision=excluded.collision,auth_kind=excluded.auth_kind,auth_id=excluded.auth_id,"
        "status=excluded.status,temporary_path=excluded.temporary_path,error_category=excluded.error_category,"
        "error_message=excluded.error_message,error_step=excluded.error_step,error_code=excluded.error_code,"
        "created_at_ms=excluded.created_at_ms,updated_at_ms=excluded.updated_at_ms";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(database->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        (void)db_error(error, database->handle, "persistência", "Falha ao preparar tarefa.");
        return false;
    }
    sqlite3_bind_text(stmt, 1, task->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, dld_task_kind_name(task->kind), -1, SQLITE_STATIC);
    bind_optional_text(stmt, 3, task->input_url);
    bind_optional_text(stmt, 4, task->input_path);
    sqlite3_bind_text(stmt, 5, task->options_json != NULL ? task->options_json : "{}", -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 6, task->destination);
    sqlite3_bind_text(stmt, 7, dld_collision_policy_name(task->collision), -1, SQLITE_STATIC);
    if (task->has_auth) {
        sqlite3_bind_text(stmt, 8, dld_auth_kind_name(task->auth.kind), -1, SQLITE_STATIC);
        bind_optional_text(stmt, 9, task->auth.id);
    } else {
        sqlite3_bind_null(stmt, 8);
        sqlite3_bind_null(stmt, 9);
    }
    sqlite3_bind_text(stmt, 10, dld_task_status_name(task->status), -1, SQLITE_STATIC);
    bind_optional_text(stmt, 11, task->temporary_path);
    if (task->has_error) {
        sqlite3_bind_int(stmt, 12, (int)task->error.category);
        bind_optional_text(stmt, 13, task->error.message);
        bind_optional_text(stmt, 14, task->error.step);
        if (task->error.has_code) sqlite3_bind_int(stmt, 15, task->error.code);
        else sqlite3_bind_null(stmt, 15);
    } else {
        for (int i = 12; i <= 15; ++i) sqlite3_bind_null(stmt, i);
    }
    sqlite3_bind_int64(stmt, 16, (sqlite3_int64)task->created_at_ms);
    sqlite3_bind_int64(stmt, 17, (sqlite3_int64)task->updated_at_ms);
    const int status = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (status != SQLITE_DONE) {
        (void)db_error(error, database->handle, "persistência", "Falha ao salvar tarefa.");
        return false;
    }
    return true;
}

static bool column_text_copy(sqlite3_stmt *stmt, int column, char **output)
{
    *output = NULL;
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) return true;
    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (text == NULL) return true;
    *output = dld_string_duplicate((const char *)text);
    return *output != NULL;
}

static bool task_from_row(sqlite3_stmt *stmt, DldTaskRecord *task)
{
    DldTaskRecord value;
    dld_task_record_init(&value);
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const char *kind = (const char *)sqlite3_column_text(stmt, 1);
    const char *collision = (const char *)sqlite3_column_text(stmt, 6);
    const char *status = (const char *)sqlite3_column_text(stmt, 9);
    if (id == NULL || kind == NULL || collision == NULL || status == NULL) goto fail;
    value.id = dld_string_duplicate(id);
    if (value.id == NULL || !dld_task_kind_from_name(kind, &value.kind) ||
        !dld_collision_policy_from_name(collision, &value.collision) ||
        !dld_task_status_from_name(status, &value.status)) goto fail;
    if (!column_text_copy(stmt, 2, &value.input_url) || !column_text_copy(stmt, 3, &value.input_path) ||
        !column_text_copy(stmt, 4, &value.options_json) || !column_text_copy(stmt, 5, &value.destination) ||
        !column_text_copy(stmt, 10, &value.temporary_path)) goto fail;
    if (value.options_json == NULL) value.options_json = dld_string_duplicate("{}");
    if (value.options_json == NULL) goto fail;

    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        const char *auth_kind = (const char *)sqlite3_column_text(stmt, 7);
        DldAuthKind parsed_kind;
        if (auth_kind == NULL || !dld_auth_kind_from_name(auth_kind, &parsed_kind)) goto fail;
        const char *auth_id = sqlite3_column_type(stmt, 8) == SQLITE_NULL ? NULL :
                              (const char *)sqlite3_column_text(stmt, 8);
        if (!dld_auth_ref_set(&value.auth, parsed_kind, auth_id)) goto fail;
        value.has_auth = true;
    }

    if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
        value.has_error = true;
        value.error.category = (DldErrorCategory)sqlite3_column_int(stmt, 11);
        if (!column_text_copy(stmt, 12, &value.error.message) ||
            !column_text_copy(stmt, 13, &value.error.step)) goto fail;
        if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) {
            value.error.has_code = true;
            value.error.code = sqlite3_column_int(stmt, 14);
        }
    }
    value.created_at_ms = (uint64_t)sqlite3_column_int64(stmt, 15);
    value.updated_at_ms = (uint64_t)sqlite3_column_int64(stmt, 16);
    dld_task_record_clear(task);
    *task = value;
    return true;
fail:
    dld_task_record_clear(&value);
    return false;
}

static const char *select_columns =
    "id,kind,input_url,input_path,options_json,destination,collision,auth_kind,auth_id,status,"
    "temporary_path,error_category,error_message,error_step,error_code,created_at_ms,updated_at_ms";

bool dld_database_get_task(DldDatabase *database, const char *id,
                           DldTaskRecord *task, bool *found, DldAppError *error)
{
    if (found != NULL) *found = false;
    if (database == NULL || database->handle == NULL || id == NULL || task == NULL) return false;
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "SELECT %s FROM task_records WHERE id=?", select_columns);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(database->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        (void)db_error(error, database->handle, "persistência", "Falha ao consultar tarefa.");
        return false;
    }
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    const int status = sqlite3_step(stmt);
    if (status == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return true;
    }
    if (status != SQLITE_ROW || !task_from_row(stmt, task)) {
        sqlite3_finalize(stmt);
        (void)db_error(error, database->handle, "persistência", "Registro de tarefa inválido.");
        return false;
    }
    sqlite3_finalize(stmt);
    if (found != NULL) *found = true;
    return true;
}

bool dld_database_list_tasks(DldDatabase *database, DldTaskRecord **tasks,
                             size_t *count, DldAppError *error)
{
    if (tasks == NULL || count == NULL || database == NULL || database->handle == NULL) return false;
    *tasks = NULL;
    *count = 0U;
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "SELECT %s FROM task_records ORDER BY updated_at_ms DESC", select_columns);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(database->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        (void)db_error(error, database->handle, "persistência", "Falha ao listar tarefas.");
        return false;
    }
    size_t capacity = 0U;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count == capacity) {
            const size_t next = capacity == 0U ? 8U : capacity * 2U;
            DldTaskRecord *grown = realloc(*tasks, next * sizeof(**tasks));
            if (grown == NULL) goto fail;
            *tasks = grown;
            capacity = next;
        }
        dld_task_record_init(&(*tasks)[*count]);
        if (!task_from_row(stmt, &(*tasks)[*count])) goto fail;
        ++*count;
    }
    sqlite3_finalize(stmt);
    return true;
fail:
    sqlite3_finalize(stmt);
    dld_database_free_task_list(*tasks, *count);
    *tasks = NULL;
    *count = 0U;
    (void)dld_app_error_set(error, DLD_ERROR_INTERNAL, "Memória insuficiente ao listar tarefas.",
                            "persistência", false, 0);
    return false;
}

void dld_database_free_task_list(DldTaskRecord *tasks, size_t count)
{
    if (tasks == NULL) return;
    for (size_t i = 0U; i < count; ++i) dld_task_record_clear(&tasks[i]);
    free(tasks);
}

bool dld_database_record_youtube_start(DldDatabase *database, uint64_t timestamp_ms,
                                       DldAppError *error)
{
    sqlite3_stmt *stmt = NULL;
    if (database == NULL || database->handle == NULL ||
        sqlite3_prepare_v2(database->handle, "INSERT INTO youtube_starts(timestamp_ms) VALUES (?)", -1,
                           &stmt, NULL) != SQLITE_OK) {
        (void)db_error(error, database != NULL ? database->handle : NULL,
                       "limite YouTube", "Falha ao registrar início.");
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)timestamp_ms);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) (void)db_error(error, database->handle, "limite YouTube", "Falha ao registrar início.");
    return ok;
}

bool dld_database_count_youtube_starts_since(DldDatabase *database, uint64_t since_ms,
                                             unsigned *count, uint64_t *oldest_ms,
                                             DldAppError *error)
{
    if (count != NULL) *count = 0U;
    if (oldest_ms != NULL) *oldest_ms = 0U;
    sqlite3_stmt *stmt = NULL;
    if (database == NULL || database->handle == NULL ||
        sqlite3_prepare_v2(database->handle,
            "SELECT COUNT(*), COALESCE(MIN(timestamp_ms),0) FROM youtube_starts WHERE timestamp_ms>=?",
            -1, &stmt, NULL) != SQLITE_OK) {
        (void)db_error(error, database != NULL ? database->handle : NULL,
                       "limite YouTube", "Falha ao consultar limite.");
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)since_ms);
    const int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        (void)db_error(error, database->handle, "limite YouTube", "Falha ao consultar limite.");
        return false;
    }
    const sqlite3_int64 db_count = sqlite3_column_int64(stmt, 0);
    if (count != NULL) *count = db_count > (sqlite3_int64)UINT_MAX ? UINT_MAX : (unsigned)db_count;
    if (oldest_ms != NULL) *oldest_ms = (uint64_t)sqlite3_column_int64(stmt, 1);
    sqlite3_finalize(stmt);
    return true;
}

bool dld_database_prune_youtube_starts(DldDatabase *database, uint64_t before_ms,
                                       DldAppError *error)
{
    sqlite3_stmt *stmt = NULL;
    if (database == NULL || database->handle == NULL ||
        sqlite3_prepare_v2(database->handle, "DELETE FROM youtube_starts WHERE timestamp_ms<?", -1,
                           &stmt, NULL) != SQLITE_OK) {
        (void)db_error(error, database != NULL ? database->handle : NULL,
                       "limite YouTube", "Falha ao limpar limite.");
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)before_ms);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) (void)db_error(error, database->handle, "limite YouTube", "Falha ao limpar limite.");
    return ok;
}
