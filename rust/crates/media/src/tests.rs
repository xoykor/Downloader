use super::*;
use std::path::Path;

#[test]
fn parses_ytdlp_fixture_and_playlist_first_entry() {
    let json = r#"{"entries":[{"id":"v1","title":"A","duration":12.5,"formats":[{"format_id":"18","ext":"mp4","vcodec":"avc1","acodec":"mp4a","width":640,"height":360,"filesize":1000}]}]}"#;
    let m = parse_ytdlp_json(json).unwrap();
    assert_eq!(m.id.as_deref(), Some("v1"));
    assert_eq!(m.formats[0].height, Some(360));
}

#[test]
fn playlist_parser_preserves_all_entries() {
    let json = r#"{"entries":[{"id":"v1"},{"id":"v2"}]}"#;
    let entries = parse_ytdlp_entries(json).unwrap();
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[1].id.as_deref(), Some("v2"));
}

#[test]
fn parses_ffprobe_without_duration_as_unknown() {
    let json = r#"{"streams":[{"index":0,"codec_type":"video","codec_name":"h264","width":1920,"height":1080}],"format":{"format_name":"mp4"}}"#;
    let p = parse_ffprobe_json(json).unwrap();
    assert_eq!(p.streams[0].codec_name.as_deref(), Some("h264"));
    assert_eq!(p.format.unwrap().duration, None);
}

#[test]
fn ytdlp_args_keep_url_and_template_as_separate_arguments() {
    let c = build_ytdlp_args(
        "https://example.test/a?x=1",
        Some("bv*+ba"),
        Path::new("/tmp/%(title)s.%(ext)s"),
    );
    assert_eq!(c.program, "yt-dlp");
    assert!(c.args.windows(2).any(|w| w == ["-f", "bv*+ba"]));
    assert_eq!(c.args.last().unwrap(), "https://example.test/a?x=1");
}

#[test]
fn analysis_args_are_structured_and_read_only() {
    let c = build_analysis_args("https://example.test/video");
    assert_eq!(c.program, "yt-dlp");
    assert!(c.args.windows(2).any(|w| w == ["-J", "--simulate"]));
    assert_eq!(c.args.last().unwrap(), "https://example.test/video");
}

#[test]
fn analysis_playlist_policy_is_explicit() {
    let item = build_analysis_args_with_playlist("https://example.test/item", false);
    let playlist = build_analysis_args_with_playlist("https://example.test/list", true);
    assert!(item.args.iter().any(|a| a == "--no-playlist"));
    assert!(!playlist.args.iter().any(|a| a == "--no-playlist"));
    assert!(playlist.args.iter().any(|a| a == "--flat-playlist"));
    assert!(playlist.args.iter().any(|a| a == "--lazy-playlist"));
    assert_eq!(playlist.args.last().unwrap(), "https://example.test/list");
}

#[test]
fn download_playlist_policy_is_explicit() {
    let item = build_ytdlp_args(
        "https://example.test/item",
        None,
        Path::new("out/%(title)s.%(ext)s"),
    );
    let playlist = build_ytdlp_args_with_playlist(
        "https://example.test/list",
        None,
        Path::new("out/%(playlist_index)s-%(title)s.%(ext)s"),
        true,
    );
    assert!(item.args.iter().any(|a| a == "--no-playlist"));
    assert!(!playlist.args.iter().any(|a| a == "--no-playlist"));
    assert_eq!(playlist.args.last().unwrap(), "https://example.test/list");
}

#[test]
fn cookie_file_is_passed_as_a_single_ytdlp_argument() {
    let auth = downloader_domain::AuthRef {
        id: "/tmp/meus cookies.txt".into(),
        kind: downloader_domain::AuthKind::CookieFile,
    };
    let command = build_ytdlp_args_with_playlist_and_auth(
        "https://example.test/private",
        None,
        Path::new("out/%(title)s.%(ext)s"),
        false,
        Some(&auth),
    )
    .unwrap();
    assert!(command
        .args
        .windows(2)
        .any(|window| window == ["--cookies", "/tmp/meus cookies.txt"]));
}

#[test]
fn browser_profile_is_supported_and_unknown_browser_is_rejected() {
    let auth = downloader_domain::AuthRef {
        id: "firefox:default-release".into(),
        kind: downloader_domain::AuthKind::BrowserProfile,
    };
    let command = build_analysis_args_with_playlist_and_auth(
        "https://example.test/private",
        false,
        Some(&auth),
    )
    .unwrap();
    assert!(command
        .args
        .windows(2)
        .any(|window| window == ["--cookies-from-browser", "firefox:default-release"]));

    let unknown = downloader_domain::AuthRef {
        id: "unknown".into(),
        kind: downloader_domain::AuthKind::BrowserProfile,
    };
    assert!(build_analysis_args_with_playlist_and_auth(
        "https://example.test/private",
        false,
        Some(&unknown),
    )
    .is_err());
}

#[test]
fn format_choices_map_to_safe_ytdlp_selectors() {
    assert_eq!(
        format_selector(FormatChoice::VideoOnly, Some(720)),
        "bestvideo[height<=720]"
    );
    assert_eq!(
        format_selector(FormatChoice::AudioOnly, None),
        "bestaudio/best"
    );
    assert!(format_selector(FormatChoice::VideoAndAudio, None).contains("+bestaudio"));
}

#[test]
fn bitrate_is_normalized_and_mapped_without_shell_fragments() {
    assert_eq!(normalize_bitrate_kbps(Some(128.4)), Some(128));
    assert_eq!(normalize_bitrate_kbps(Some(f64::NAN)), None);
    assert_eq!(normalize_bitrate_kbps(Some(0.0)), None);
    assert_eq!(
        bitrate_selector(BitrateChoice::Kbps(192)),
        Some("[tbr<=192]".into())
    );
    assert_eq!(bitrate_selector(BitrateChoice::Auto), None);
    assert_eq!(
        ffmpeg_bitrate_args(BitrateChoice::Kbps(320), true),
        vec!["-b:a", "320k"]
    );
    assert_eq!(
        ffmpeg_bitrate_args(BitrateChoice::Kbps(2000), false),
        vec!["-b:v", "2000k"]
    );
    assert!(ffmpeg_bitrate_args(BitrateChoice::Kbps(0), true).is_empty());
}

#[test]
fn local_conversion_builder_reuses_validated_software_plan() {
    let plan = preset_copy(Container::Mp4, Some(0), Some(1));
    let c = build_conversion_args(Path::new("input.mkv"), Path::new("output.mp4"), &plan).unwrap();
    assert_eq!(c.program, "ffmpeg");
    assert!(!c.args.iter().any(|a| a == "-hwaccel"));
    assert_eq!(c.args.last().unwrap(), "output.mp4");
}

#[test]
fn ffmpeg_places_input_before_maps_codecs_and_output() {
    let plan = preset_copy(Container::Mp4, Some(0), Some(1));
    let c = build_ffmpeg_args(Path::new("in file.webm"), Path::new("out.mp4"), &plan).unwrap();
    let input = c.args.iter().position(|x| x == "-i").unwrap();
    let map = c.args.iter().position(|x| x == "-map").unwrap();
    let output = c.args.iter().position(|x| x == "out.mp4").unwrap();
    assert!(input < map && map < output);
    assert!(c.args.windows(2).any(|w| w == ["-c:v", "copy"]));
}

#[test]
fn ffmpeg_defaults_unspecified_audio_action_to_copy() {
    let mut plan = preset_copy(Container::Mkv, Some(0), Some(1));
    plan.audio_action = None;
    let c = build_ffmpeg_args(Path::new("in.mkv"), Path::new("out.mkv"), &plan).unwrap();
    assert!(c.args.windows(2).any(|w| w == ["-c:a", "copy"]));
}

#[test]
fn acceleration_adds_vulkan_only_after_validation() {
    let mut plan = preset_copy(Container::Mp4, Some(0), Some(1));
    plan.video_action = Some(StreamAction::Reencode);
    plan.video_codec = Some("h264".into());
    let software = build_ffmpeg_args_with_acceleration(
        Path::new("in.mp4"),
        Path::new("out.mp4"),
        &plan,
        AccelerationMode::Vulkan,
        false,
    )
    .unwrap();
    assert!(!software.args.iter().any(|a| a == "-hwaccel"));
    assert!(software.args.windows(2).any(|w| w == ["-c:v", "h264"]));
    let vulkan = build_ffmpeg_args_with_acceleration(
        Path::new("in.mp4"),
        Path::new("out.mp4"),
        &plan,
        AccelerationMode::Vulkan,
        true,
    )
    .unwrap();
    assert!(vulkan.args.windows(2).any(|w| w == ["-hwaccel", "vulkan"]));
    assert!(vulkan.args.windows(2).any(|w| w == ["-c:v", "h264_vulkan"]));

    plan.video_codec = Some("libx264".into());
    let vulkan = build_ffmpeg_args_with_acceleration(
        Path::new("in.mp4"),
        Path::new("out.mp4"),
        &plan,
        AccelerationMode::Vulkan,
        true,
    )
    .unwrap();
    assert!(vulkan.args.windows(2).any(|w| w == ["-c:v", "h264_vulkan"]));
}

#[test]
fn plan_rejects_unknown_or_incompatible_codec() {
    let mut p = preset_copy(Container::Webm, Some(0), Some(1));
    p.video_action = Some(StreamAction::Reencode);
    p.video_codec = Some("h264".into());
    assert!(p.validate().is_err());
}

#[test]
fn converts_media_and_plan_to_shared_domain_models() {
    let response = parse_ytdlp_json(r#"{"id":"v1","title":"A","duration":1.25,"formats":[{"format_id":"18","ext":"mp4","vcodec":"h264","acodec":"aac","tbr":128.0}]}"#).unwrap();
    let info = response.to_media_info();
    assert_eq!(info.duration_ms, Some(1250));
    assert_eq!(info.formats[0].id, "18");
    let plan = preset_copy(Container::Mp4, Some(0), Some(1));
    let shared = plan.to_domain().unwrap();
    assert_eq!(shared.selected_streams, vec![0, 1]);
    assert!(shared.copy_video && shared.copy_audio);
}

#[test]
fn converts_ffprobe_streams_and_ratios() {
    let response = parse_ffprobe_json(r#"{"streams":[{"index":2,"codec_type":"video","codec_name":"h264","r_frame_rate":"30000/1001","bit_rate":"1000"}]}"#).unwrap();
    let streams = response.to_stream_info();
    assert_eq!(streams[0].index, 2);
    assert!((streams[0].fps.unwrap() - 29.97).abs() < 0.01);
}

#[test]
fn progress_is_tolerant_and_preserves_missing_values() {
    let p = parse_progress_line("percent=42.5 elapsed=3 speed=1.2");
    assert_eq!(p.percent, Some(42.5));
    assert_eq!(p.elapsed_seconds, Some(3.0));
    assert_eq!(p.eta_seconds, None);
}
