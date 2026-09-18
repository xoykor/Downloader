#include "downloader/hardware.h"
#include "downloader/process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *mode_name(DldAccelerationMode mode)
{
    switch (mode) {
    case DLD_ACCELERATION_VULKAN: return "vulkan";
    case DLD_ACCELERATION_VAAPI: return "vaapi";
    case DLD_ACCELERATION_AMF: return "amf";
    case DLD_ACCELERATION_CUDA: return "cuda";
    case DLD_ACCELERATION_QSV: return "qsv";
    default: return NULL;
    }
}

void dld_hardware_probe_clear(DldHardwareProbe *probe)
{
    if (probe == NULL) return;
    free(probe->detail);
    memset(probe, 0, sizeof(*probe));
}

static bool output_lists_backend(const char *output, const char *backend)
{
    if (output == NULL || backend == NULL) return false;
    const size_t length = strlen(backend);
    const char *line = output;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const size_t line_len = end == NULL ? strlen(line) : (size_t)(end - line);
        if (line_len == length && strncmp(line, backend, length) == 0) return true;
        if (end == NULL) break;
        line = end + 1;
    }
    return false;
}

static char *last_line_copy(const char *text)
{
    if (text == NULL || *text == '\0') return NULL;
    const char *end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r')) --end;
    const char *start = end;
    while (start > text && start[-1] != '\n') --start;
    const size_t length = (size_t)(end - start);
    char *copy = malloc(length + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

bool dld_hardware_probe(const char *ffmpeg, DldAccelerationMode mode,
                        DldHardwareProbe *probe, DldAppError *error)
{
    if (probe == NULL || ffmpeg == NULL) return false;
    dld_hardware_probe_clear(probe);
    probe->mode = mode;
    const char *backend = mode_name(mode);
    if (backend == NULL) {
        probe->advertised = mode == DLD_ACCELERATION_SOFTWARE;
        probe->usable = mode == DLD_ACCELERATION_SOFTWARE;
        return true;
    }

    char *hwargv[] = {(char *)ffmpeg, "-hide_banner", "-hwaccels", NULL};
    DldProcessSpec list_spec = {.program = ffmpeg, .argv = hwargv, .timeout_ms = 3000U};
    DldProcessResult listed;
    dld_process_result_init(&listed);
    if (!dld_process_run(&list_spec, NULL, NULL, NULL, &listed, error)) {
        dld_process_result_clear(&listed);
        return false;
    }
    probe->advertised = listed.exit_code == 0 && output_lists_backend(listed.stdout_text, backend);
    dld_process_result_clear(&listed);
    if (!probe->advertised) {
        probe->detail = dld_string_duplicate("Backend não anunciado pelo FFmpeg.");
        return true;
    }

    char device[64];
    (void)snprintf(device, sizeof(device), "%s=list", backend);
    char *argv[] = {
        (char *)ffmpeg, "-hide_banner", "-init_hw_device", device,
        "-f", "lavfi", "-i", "nullsrc", "-frames:v", "1",
        "-f", "null", "-", NULL
    };
    DldProcessSpec probe_spec = {.program = ffmpeg, .argv = argv, .timeout_ms = 3000U};
    DldProcessResult result;
    dld_process_result_init(&result);
    if (!dld_process_run(&probe_spec, NULL, NULL, NULL, &result, error)) {
        dld_process_result_clear(&result);
        return false;
    }
    probe->usable = result.exited && result.exit_code == 0 && !result.timed_out && !result.cancelled;
    probe->detail = last_line_copy(result.stderr_text);
    dld_process_result_clear(&result);
    return true;
}

DldAccelerationMode dld_hardware_resolve_auto(const char *ffmpeg)
{
    static const DldAccelerationMode preferred[] = {
        DLD_ACCELERATION_VAAPI,
        DLD_ACCELERATION_VULKAN,
        DLD_ACCELERATION_QSV,
        DLD_ACCELERATION_CUDA,
        DLD_ACCELERATION_AMF,
    };
    for (size_t i = 0U; i < sizeof(preferred) / sizeof(preferred[0]); ++i) {
        DldHardwareProbe probe = {0};
        DldAppError error;
        dld_app_error_init(&error);
        const bool ok = dld_hardware_probe(ffmpeg, preferred[i], &probe, &error);
        const bool usable = ok && probe.usable;
        dld_hardware_probe_clear(&probe);
        dld_app_error_clear(&error);
        if (usable) return preferred[i];
    }
    return DLD_ACCELERATION_SOFTWARE;
}
