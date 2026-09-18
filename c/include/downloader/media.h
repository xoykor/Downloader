#ifndef DOWNLOADER_MEDIA_H
#define DOWNLOADER_MEDIA_H

/*
 * Planejamento de mídia, parsing e construção de argv.
 *
 * `DldCommand` possui o vetor `argv` e cada string contida nele. Depois de usar o
 * comando, sempre chame `dld_command_clear`. Nenhum comando é concatenado numa
 * linha de shell: cada argumento continua sendo um elemento separado de `argv`.
 */

#include "downloader/domain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char **argv;
    size_t argc;
} DldCommand;

typedef struct {
    char *id;
    char *title;
    char *uploader;
    double duration_seconds;
    bool has_duration;
    bool is_playlist;
    size_t playlist_entries;
} DldMediaSummary;

typedef struct {
    bool valid;
    size_t video_streams;
    size_t audio_streams;
    double duration_seconds;
    bool has_duration;
    char *container;
} DldProbeSummary;

typedef struct {
    bool has_percent;
    double percent;
    bool has_eta;
    double eta_seconds;
    char speed[64];
} DldProgress;

typedef enum {
    DLD_TRACK_LINE_NONE = 0,
    DLD_TRACK_LINE_PROGRESS,
    DLD_TRACK_LINE_FILE
} DldTrackLineKind;

/*
 * Evento estruturado emitido pelo yt-dlp para uma faixa individual.
 *
 * Campos textuais usam buffers próprios para que o parser não devolva ponteiros
 * para a linha temporária recebida do processo.
 */
typedef struct {
    DldTrackLineKind kind;
    char id[160];
    char title[512];
    char filepath[1024];
    size_t playlist_index;
    size_t playlist_count;
    DldProgress progress;
} DldTrackLine;

void dld_command_init(DldCommand *command);
void dld_command_clear(DldCommand *command);
void dld_media_summary_init(DldMediaSummary *summary);
void dld_media_summary_clear(DldMediaSummary *summary);
void dld_probe_summary_init(DldProbeSummary *summary);
void dld_probe_summary_clear(DldProbeSummary *summary);

bool dld_validate_url(const char *url);
bool dld_validate_netscape_cookie_file(const char *path, DldAppError *error);
bool dld_validate_browser_selector(const char *selector, DldAppError *error);

/* Construtores de argv: todos retornam argumentos independentes, sem shell. */
bool dld_build_analysis_command(const char *yt_dlp, const char *url, bool playlist,
                                const DldAuthRef *auth, DldCommand *command,
                                DldAppError *error);
bool dld_build_download_command(const char *yt_dlp, const char *url,
                                const char *output_template, bool playlist,
                                const char *media_kind, const char *format,
                                unsigned max_height, const char *bitrate,
                                bool youtube_protection, unsigned youtube_allowance,
                                const DldAuthRef *auth,
                                DldCommand *command, DldAppError *error);
bool dld_build_ffprobe_command(const char *ffprobe, const char *input,
                               DldCommand *command, DldAppError *error);
bool dld_build_conversion_command(const char *ffmpeg, const char *input,
                                  const char *output, const char *format,
                                  DldAccelerationMode acceleration,
                                  bool acceleration_validated,
                                  DldCommand *command, DldAppError *error);

bool dld_parse_ytdlp_summary(const char *json_text, DldMediaSummary *summary,
                             DldAppError *error);
bool dld_parse_ffprobe_summary(const char *json_text, DldProbeSummary *summary,
                               DldAppError *error);
DldProgress dld_parse_progress_line(const char *line);
bool dld_parse_track_line(const char *line, DldTrackLine *track);

/* Helpers de opções JSON usados por engine/desktop. */
char *dld_json_get_string_copy(const char *json_text, const char *key,
                               const char *fallback);
bool dld_json_get_bool(const char *json_text, const char *key, bool fallback);
int dld_json_get_int(const char *json_text, const char *key, int fallback);

#ifdef __cplusplus
}
#endif

#endif
