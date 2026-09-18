#ifndef DOWNLOADER_DOMAIN_H
#define DOWNLOADER_DOMAIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tipos compartilhados pelo núcleo, CLI e interface gráfica.
 *
 * Esta camada não acessa rede, disco, banco ou processos. Manter os contratos
 * independentes da infraestrutura reduz acoplamento e torna os testes baratos.
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
    DLD_AUTH_NONE = 0,
    DLD_AUTH_COOKIE_FILE,
    DLD_AUTH_BROWSER_PROFILE
} DldAuthKind;

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
    DldAuthKind kind;
    char *id;
} DldAuthRef;

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

    /* JSON é mantido como texto para preservar compatibilidade e extensibilidade. */
    char *options_json;

    char *destination;
    DldCollisionPolicy collision;
    bool has_auth;
    DldAuthRef auth;
    DldTaskStatus status;
    char *temporary_path;

    bool has_error;
    DldAppError error;

    uint64_t created_at_ms;
    uint64_t updated_at_ms;
} DldTaskRecord;

bool dld_task_status_is_terminal(DldTaskStatus status);
bool dld_task_status_is_active(DldTaskStatus status);

const char *dld_task_kind_name(DldTaskKind kind);
bool dld_task_kind_from_name(const char *name, DldTaskKind *kind);
const char *dld_task_status_name(DldTaskStatus status);
bool dld_task_status_from_name(const char *name, DldTaskStatus *status);
const char *dld_collision_policy_name(DldCollisionPolicy policy);
bool dld_collision_policy_from_name(const char *name, DldCollisionPolicy *policy);
const char *dld_auth_kind_name(DldAuthKind kind);
bool dld_auth_kind_from_name(const char *name, DldAuthKind *kind);

void dld_auth_ref_init(DldAuthRef *auth);
void dld_auth_ref_clear(DldAuthRef *auth);
bool dld_auth_ref_set(DldAuthRef *auth, DldAuthKind kind, const char *id);
bool dld_auth_ref_copy(DldAuthRef *destination, const DldAuthRef *source);

void dld_app_error_init(DldAppError *error);
void dld_app_error_clear(DldAppError *error);
bool dld_app_error_set(DldAppError *error, DldErrorCategory category,
                       const char *message, const char *step,
                       bool has_code, int code);
bool dld_app_error_copy(DldAppError *destination, const DldAppError *source);

void dld_task_record_init(DldTaskRecord *task);
void dld_task_record_clear(DldTaskRecord *task);
bool dld_task_record_copy(DldTaskRecord *destination, const DldTaskRecord *source);

/* Utilitário C17 usado pelos módulos que precisam assumir ownership de texto. */
char *dld_string_duplicate(const char *text);

#ifdef __cplusplus
}
#endif

#endif
