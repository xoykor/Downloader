#include "downloader/media.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    DldAppError error; dld_app_error_init(&error);
    DldMediaSummary media; dld_media_summary_init(&media);
    const char *json = "{\"id\":\"abc\",\"title\":\"Teste\",\"uploader\":\"Canal\",\"duration\":12.5}";
    assert(dld_parse_ytdlp_summary(json, &media, &error));
    assert(strcmp(media.id, "abc") == 0);
    assert(strcmp(media.title, "Teste") == 0);
    assert(media.has_duration);

    DldProbeSummary probe; dld_probe_summary_init(&probe);
    const char *probe_json = "{\"streams\":[{\"codec_type\":\"video\"},{\"codec_type\":\"audio\"}],\"format\":{\"format_name\":\"matroska\",\"duration\":\"2.0\"}}";
    assert(dld_parse_ffprobe_summary(probe_json, &probe, &error));
    assert(probe.video_streams == 1U && probe.audio_streams == 1U);

    DldCommand command; dld_command_init(&command);
    assert(dld_build_download_command("yt-dlp", "https://example.test/x", "/tmp/%(id)s.%(ext)s",
                                      false, "audio", "opus", 0U, "128K", true, 300U,
                                      NULL, &command, &error));
    bool saw_no_playlist = false, saw_audio_format = false;
    for (size_t i = 0; i < command.argc; ++i) {
        if (strcmp(command.argv[i], "--no-playlist") == 0) saw_no_playlist = true;
        if (strcmp(command.argv[i], "--audio-format") == 0) saw_audio_format = true;
    }
    assert(saw_no_playlist && saw_audio_format);
    DldProgress progress = dld_parse_progress_line("percent=42.5% eta=8 speed=1.2MiB/s");
    assert(progress.has_percent && progress.percent > 42.4 && progress.percent < 42.6);
    assert(progress.has_eta);

    dld_command_clear(&command);

    dld_command_init(&command);
    assert(dld_build_download_command("yt-dlp", "https://example.test/playlist",
                                      "/tmp/%(id)s.%(ext)s", true,
                                      "video+audio", "auto", 0U, "auto", true, 42U,
                                      NULL, &command, &error));
    bool saw_yes_playlist = false;
    bool saw_ignore_errors = false;
    bool saw_track_template = false;
    bool saw_after_move = false;
    bool saw_before_dl = false;
    bool saw_max_downloads = false;
    bool saw_ignore_config = false;
    bool saw_no_simulate = false;
    for (size_t i = 0; i < command.argc; ++i) {
        if (strcmp(command.argv[i], "--yes-playlist") == 0) saw_yes_playlist = true;
        if (strcmp(command.argv[i], "--ignore-errors") == 0) saw_ignore_errors = true;
        if (strncmp(command.argv[i], "TRACK ", 6U) == 0) saw_track_template = true;
        if (strncmp(command.argv[i], "after_move:FILE ", 16U) == 0) saw_after_move = true;
        if (strncmp(command.argv[i], "before_dl:POLICY_VIDEO ", 23U) == 0) saw_before_dl = true;
        if (strcmp(command.argv[i], "--max-downloads") == 0 &&
            i + 1U < command.argc &&
            strcmp(command.argv[i + 1U], "42") == 0) {
            saw_max_downloads = true;
        }
        if (strcmp(command.argv[i], "--ignore-config") == 0) saw_ignore_config = true;
        if (strcmp(command.argv[i], "--no-simulate") == 0) saw_no_simulate = true;
    }
    assert(saw_yes_playlist && saw_ignore_errors);
    assert(saw_track_template && saw_after_move);
    assert(saw_before_dl && saw_max_downloads);
    assert(saw_ignore_config && saw_no_simulate);

    DldTrackLine track;
    assert(dld_parse_track_line(
        "TRACK {\"id\":\"abc\",\"title\":\"Minha faixa\","
        "\"playlist_index\":2,\"playlist_count\":8} "
        "{\"_percent_str\":\"37.5%\",\"_speed_str\":\"2.1MiB/s\","
        "\"eta\":12,\"status\":\"downloading\"}",
        &track));
    assert(track.kind == DLD_TRACK_LINE_PROGRESS);
    assert(strcmp(track.id, "abc") == 0);
    assert(strcmp(track.title, "Minha faixa") == 0);
    assert(track.playlist_index == 2U && track.playlist_count == 8U);
    assert(track.progress.has_percent);
    assert(track.progress.percent > 37.4 && track.progress.percent < 37.6);
    assert(strcmp(track.progress.speed, "2.1MiB/s") == 0);

    assert(dld_parse_track_line(
        "FILE {\"id\":\"abc\",\"title\":\"Minha faixa\","
        "\"playlist_index\":2,\"playlist_count\":8,"
        "\"filepath\":\"/tmp/Minha faixa [abc].webm\"}",
        &track));
    assert(track.kind == DLD_TRACK_LINE_FILE);
    assert(strcmp(track.filepath, "/tmp/Minha faixa [abc].webm") == 0);

    dld_command_clear(&command); dld_probe_summary_clear(&probe);
    dld_media_summary_clear(&media); dld_app_error_clear(&error);
    puts("test_media: ok");
    return 0;
}
