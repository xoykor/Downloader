#ifndef DOWNLOADER_DOMAIN_H
#define DOWNLOADER_DOMAIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tipos de domínio compartilhados por CLI, interface gráfica e serviços.
 *
 * Esta camada não executa processos, não acessa rede e não abre banco de dados.
 * A ideia é manter aqui apenas dados e regras simples de estado, espelhando o
 * papel do crate `downloader-domain` da implementação Rust.
 */

typedef enum {
    DLD_ACCELERATION_AUTO = 0,
    DLD_ACCELERATION_SOFTWARE,
    DLD_ACCELERATION_VULKAN,
    DLD_ACCELERATION_VAAPI,
    DLD_ACCELERATION_AMF,
    DLD_ACCELERATION_CUDA,
    DLD_ACCELERATION_QSV
} DldAccelerationMode;

typedef enum {
    DLD_TASK_DOWNLOAD = 0,
    DLD_TASK_MERGE,
    DLD_TASK_CONVERT,
    DLD_TASK_VALIDATE
} DldTaskKind;

typedef enum {
    DLD_STATUS_QUEUED = 0,
    DLD_STATUS_ANALYZING,
    DLD_STATUS_WAITING_AUTH,
    DLD_STATUS_DOWNLOADING,
    DLD_STATUS_MERGING,
    DLD_STATUS_CONVERTING,
    DLD_STATUS_VALIDATING,
    DLD_STATUS_COMPLETED,
    DLD_STATUS_FAILED,
    DLD_STATUS_CANCELLED,
    DLD_STATUS_INTERRUPTED
} DldTaskStatus;

typedef enum {
    DLD_COLLISION_RENAME = 0,
    DLD_COLLISION_REPLACE,
    DLD_COLLISION_SKIP
} DldCollisionPolicy;

typedef enum {
    DLD_ERROR_INVALID_URL = 0,
    DLD_ERROR_AUTHENTICATION,
    DLD_ERROR_DEPENDENCY,
    DLD_ERROR_NETWORK,
    DLD_ERROR_FORMAT_UNAVAILABLE,
    DLD_ERROR_ENCODER_UNAVAILABLE,
    DLD_ERROR_PERMISSION,
    DLD_ERROR_DISK_SPACE,
    DLD_ERROR_INVALID_MEDIA,
    DLD_ERROR_CANCELLED,
    DLD_ERROR_TIMEOUT,
    DLD_ERROR_INTERNAL
} DldErrorCategory;

typedef struct {
    DldErrorCategory category;
    char *message;
    char *step;
    bool has_code;
    int code;
} DldAppError;

typedef struct {
    char *id;
    DldTaskKind kind;
    char *input_url;
    char *input_path;

    /*
     * Durante a migração, as opções continuam armazenadas como um único JSON.
     * Isso mantém compatibilidade conceitual com o Rust e evita duplicar um
     * grande número de campos antes da camada JSON ser portada.
     */
    char *options_json;

    char *destination;
    DldCollisionPolicy collision;
    DldTaskStatus status;
    char *temporary_path;

    bool has_error;
    DldAppError error;

    uint64_t created_at_ms;
    uint64_t updated_at_ms;
} DldTaskRecord;

/* Regras de estado mantidas equivalentes às da versão Rust. */
bool dld_task_status_is_terminal(DldTaskStatus status);
bool dld_task_status_is_active(DldTaskStatus status);

/* Conversões apenas para logs, testes e UI. As strings retornadas são estáticas. */
const char *dld_task_kind_name(DldTaskKind kind);
const char *dld_task_status_name(DldTaskStatus status);

/* Gerenciamento explícito de memória para os tipos que possuem strings. */
void dld_app_error_init(DldAppError *error);
void dld_app_error_clear(DldAppError *error);
bool dld_app_error_set(
    DldAppError *error,
    DldErrorCategory category,
    const char *message,
    const char *step,
    bool has_code,
    int code
);
bool dld_app_error_copy(DldAppError *destination, const DldAppError *source);

void dld_task_record_init(DldTaskRecord *task);
void dld_task_record_clear(DldTaskRecord *task);
bool dld_task_record_copy(DldTaskRecord *destination, const DldTaskRecord *source);

#ifdef __cplusplus
}
#endif

#endif
