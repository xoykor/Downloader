#ifndef DOWNLOADER_HARDWARE_H
#define DOWNLOADER_HARDWARE_H

/*
 * Sondagem de aceleração de vídeo.
 *
 * "anunciado" significa que o FFmpeg conhece o backend; "usável" significa que
 * uma sondagem curta realmente conseguiu inicializar o dispositivo/driver.
 */

#include "downloader/domain.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    DldAccelerationMode mode;
    bool advertised;
    bool usable;
    char *detail;
} DldHardwareProbe;

void dld_hardware_probe_clear(DldHardwareProbe *probe);
bool dld_hardware_probe(const char *ffmpeg, DldAccelerationMode mode,
                        DldHardwareProbe *probe, DldAppError *error);
DldAccelerationMode dld_hardware_resolve_auto(const char *ffmpeg);

#ifdef __cplusplus
}
#endif

#endif
