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
                                      false, "audio", "opus", 0U, "128K", true,
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

    dld_command_clear(&command); dld_probe_summary_clear(&probe);
    dld_media_summary_clear(&media); dld_app_error_clear(&error);
    puts("test_media: ok");
    return 0;
}
