//! Contratos e planejadores puros para yt-dlp, ffprobe e FFmpeg.
//!
//! O crate não executa processos nem acessa a rede. A aplicação fornece o
//! executor e publica o arquivo temporário depois de validar o resultado.

use downloader_domain as domain;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct YtDlpResponse {
    pub id: Option<String>,
    pub title: Option<String>,
    pub uploader: Option<String>,
    pub duration: Option<f64>,
    #[serde(default)]
    pub formats: Vec<YtDlpFormat>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct YtDlpFormat {
    pub format_id: Option<String>,
    pub ext: Option<String>,
    pub vcodec: Option<String>,
    pub acodec: Option<String>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub fps: Option<f64>,
    pub tbr: Option<f64>,
    pub filesize: Option<u64>,
    pub filesize_approx: Option<u64>,
    pub language: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FfprobeResponse {
    #[serde(default)]
    pub streams: Vec<FfprobeStream>,
    pub format: Option<FfprobeFormat>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FfprobeStream {
    pub index: Option<u32>,
    pub codec_type: Option<String>,
    pub codec_name: Option<String>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub r_frame_rate: Option<String>,
    pub sample_rate: Option<String>,
    pub channels: Option<u32>,
    pub bit_rate: Option<String>,
    pub tags: Option<StreamTags>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct StreamTags {
    pub language: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FfprobeFormat {
    pub format_name: Option<String>,
    pub duration: Option<String>,
    pub size: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum MediaParseError {
    InvalidJson(String),
    WrongShape(String),
}

/// Parses one media object. For a playlist response this intentionally returns
/// the first entry; callers that need every item should use
/// [`parse_ytdlp_entries`].
pub fn parse_ytdlp_json(input: &str) -> Result<YtDlpResponse, MediaParseError> {
    let value: serde_json::Value =
        serde_json::from_str(input).map_err(|e| MediaParseError::InvalidJson(e.to_string()))?;
    let value = value
        .get("entries")
        .and_then(|v| v.as_array())
        .and_then(|v| v.first())
        .unwrap_or(&value);
    serde_json::from_value(value.clone()).map_err(|e| MediaParseError::WrongShape(e.to_string()))
}

/// Parses a yt-dlp response while preserving every playlist entry.
pub fn parse_ytdlp_entries(input: &str) -> Result<Vec<YtDlpResponse>, MediaParseError> {
    let value: serde_json::Value =
        serde_json::from_str(input).map_err(|e| MediaParseError::InvalidJson(e.to_string()))?;
    let values = value
        .get("entries")
        .and_then(|v| v.as_array())
        .cloned()
        .unwrap_or_else(|| vec![value]);
    values
        .into_iter()
        .map(|item| {
            serde_json::from_value(item).map_err(|e| MediaParseError::WrongShape(e.to_string()))
        })
        .collect()
}

pub fn parse_ffprobe_json(input: &str) -> Result<FfprobeResponse, MediaParseError> {
    serde_json::from_str(input).map_err(|e| MediaParseError::InvalidJson(e.to_string()))
}

fn millis(seconds: Option<f64>) -> Option<u64> {
    seconds
        .filter(|v| v.is_finite() && *v >= 0.0)
        .map(|v| (v * 1000.0).round() as u64)
}

impl YtDlpResponse {
    /// Converts yt-dlp metadata into the shared domain snapshot.
    pub fn to_media_info(&self) -> domain::MediaInfo {
        domain::MediaInfo {
            title: self.title.clone(),
            uploader: self.uploader.clone(),
            duration_ms: millis(self.duration),
            extractor: None,
            formats: self
                .formats
                .iter()
                .filter_map(|f| {
                    let id = f.format_id.clone()?;
                    Some(domain::FormatInfo {
                        id,
                        container: f.ext.clone(),
                        video_codec: f.vcodec.clone().filter(|v| v != "none"),
                        audio_codec: f.acodec.clone().filter(|v| v != "none"),
                        width: f.width,
                        height: f.height,
                        fps: f.fps,
                        bitrate: normalize_bitrate_kbps(f.tbr).map(|v| u64::from(v) * 1000),
                        language: f.language.clone(),
                        has_video: f.vcodec.as_deref().is_some_and(|v| v != "none"),
                        has_audio: f.acodec.as_deref().is_some_and(|v| v != "none"),
                        size_bytes: f.filesize.or(f.filesize_approx),
                        size_is_estimate: f.filesize.is_none() && f.filesize_approx.is_some(),
                    })
                })
                .collect(),
            streams: Vec::new(),
            playlist: None,
        }
    }
}

impl FfprobeResponse {
    /// Converts ffprobe streams into the shared domain representation.
    pub fn to_stream_info(&self) -> Vec<domain::StreamInfo> {
        self.streams
            .iter()
            .enumerate()
            .map(|(position, s)| {
                let kind = match s.codec_type.as_deref() {
                    Some("video") => domain::StreamKind::Video,
                    Some("audio") => domain::StreamKind::Audio,
                    Some("subtitle") => domain::StreamKind::Subtitle,
                    _ => domain::StreamKind::Other,
                };
                domain::StreamInfo {
                    index: s.index.unwrap_or(position as u32),
                    kind,
                    codec: s.codec_name.clone(),
                    language: s.tags.as_ref().and_then(|t| t.language.clone()),
                    width: s.width,
                    height: s.height,
                    fps: parse_ratio(s.r_frame_rate.as_deref()),
                    bitrate: s.bit_rate.as_deref().and_then(|v| v.parse().ok()),
                    channels: s.channels.map(|v| v as u16),
                    sample_rate: s.sample_rate.as_deref().and_then(|v| v.parse().ok()),
                    duration_ms: self
                        .format
                        .as_ref()
                        .and_then(|f| f.duration.as_deref())
                        .and_then(|v| v.parse::<f64>().ok())
                        .and_then(|v| millis(Some(v))),
                }
            })
            .collect()
    }
}

fn parse_ratio(value: Option<&str>) -> Option<f64> {
    let value = value?;
    if let Ok(n) = value.parse() {
        return Some(n);
    }
    let (a, b) = value.split_once('/')?;
    let (a, b): (f64, f64) = (a.parse().ok()?, b.parse().ok()?);
    (b != 0.0).then_some(a / b)
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum StreamAction {
    Copy,
    Reencode,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum Container {
    Mp4,
    Mkv,
    Webm,
    Mp3,
    M4a,
    Flac,
    Wav,
}
impl Container {
    pub fn extension(&self) -> &'static str {
        match self {
            Self::Mp4 => "mp4",
            Self::Mkv => "mkv",
            Self::Webm => "webm",
            Self::Mp3 => "mp3",
            Self::M4a => "m4a",
            Self::Flac => "flac",
            Self::Wav => "wav",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct OutputPlan {
    pub container: Container,
    pub video_codec: Option<String>,
    pub audio_codec: Option<String>,
    pub video_action: Option<StreamAction>,
    pub audio_action: Option<StreamAction>,
    pub map_video: Option<u32>,
    pub map_audio: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlanError(pub String);
impl OutputPlan {
    pub fn validate(&self) -> Result<(), PlanError> {
        if self.map_video.is_none() && self.map_audio.is_none() {
            return Err(PlanError("nenhum stream selecionado".into()));
        }
        if self.video_action == Some(StreamAction::Reencode) && self.video_codec.is_none() {
            return Err(PlanError("encoder de vídeo ausente".into()));
        }
        if self.audio_action == Some(StreamAction::Reencode) && self.audio_codec.is_none() {
            return Err(PlanError("encoder de áudio ausente".into()));
        }
        let vc = self.video_codec.as_deref().map(|v| v.to_ascii_lowercase());
        let ac = self.audio_codec.as_deref().map(|v| v.to_ascii_lowercase());
        let ok = match self.container {
            Container::Mp4 => {
                vc.as_deref()
                    .is_none_or(|v| ["h264", "hevc", "av1", "libx264", "libx265"].contains(&v))
                    && ac
                        .as_deref()
                        .is_none_or(|v| ["aac", "alac", "mp3"].contains(&v))
            }
            Container::Webm => {
                vc.as_deref()
                    .is_none_or(|v| ["vp8", "vp9", "av1", "libvpx", "libvpx-vp9"].contains(&v))
                    && ac
                        .as_deref()
                        .is_none_or(|v| ["opus", "vorbis", "libopus"].contains(&v))
            }
            Container::Mp3 => {
                vc.is_none()
                    && ac
                        .as_deref()
                        .is_none_or(|v| ["mp3", "libmp3lame"].contains(&v))
            }
            Container::M4a => {
                vc.is_none() && ac.as_deref().is_none_or(|v| ["aac", "alac"].contains(&v))
            }
            Container::Flac => vc.is_none() && ac.as_deref().is_none_or(|v| v == "flac"),
            Container::Wav => vc.is_none() && ac.as_deref().is_none_or(|v| v == "pcm_s16le"),
            Container::Mkv => true,
        };
        if ok {
            Ok(())
        } else {
            Err(PlanError("codec incompatível com o contêiner".into()))
        }
    }

    /// Converts this media planner into the shared application contract.
    pub fn to_domain(&self) -> Result<domain::OutputPlan, PlanError> {
        self.validate()?;
        let mut selected_streams = Vec::new();
        if let Some(index) = self.map_video {
            selected_streams.push(index);
        }
        if let Some(index) = self.map_audio {
            selected_streams.push(index);
        }
        Ok(domain::OutputPlan {
            source_container: None,
            output_container: self.container.extension().to_owned(),
            selected_streams,
            video_codec: self.video_codec.clone(),
            audio_codec: self.audio_codec.clone(),
            copy_video: self
                .map_video
                .is_none_or(|_| self.video_action != Some(StreamAction::Reencode)),
            copy_audio: self
                .map_audio
                .is_none_or(|_| self.audio_action != Some(StreamAction::Reencode)),
            expected_duration_ms: None,
            expected_width: None,
            expected_height: None,
            stages: vec![
                domain::PlanStage::Convert,
                domain::PlanStage::Validate,
                domain::PlanStage::Publish,
            ],
        })
    }
}

pub fn preset_copy(container: Container, video: Option<u32>, audio: Option<u32>) -> OutputPlan {
    OutputPlan {
        container,
        video_codec: None,
        audio_codec: None,
        video_action: video.map(|_| StreamAction::Copy),
        audio_action: audio.map(|_| StreamAction::Copy),
        map_video: video,
        map_audio: audio,
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommandArgs {
    /// Argumentos separados, passados diretamente ao executor (sem shell).
    pub program: String,
    pub args: Vec<String>,
}

/// Erro ao transformar uma referência de autenticação em argumentos do
/// `yt-dlp`. O conteúdo dos cookies nunca é incorporado à mensagem.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthArgsError(pub String);

/// Acrescenta autenticação suportada pelo `yt-dlp` sem passar por shell.
///
/// `AuthRef::CookieFile` usa um arquivo Netscape (`cookies.txt`), enquanto
/// `AuthRef::BrowserProfile` usa a sintaxe do `yt-dlp`, por exemplo
/// `firefox` ou `chrome:Default`. A validação do arquivo em si fica na
/// infraestrutura, que pode acessar o sistema de arquivos.
pub fn append_auth_args(
    args: &mut Vec<String>,
    auth: Option<&domain::AuthRef>,
) -> Result<(), AuthArgsError> {
    let Some(auth) = auth else {
        return Ok(());
    };
    let id = auth.id.trim();
    if id.is_empty() || id.contains('\0') {
        return Err(AuthArgsError("referência de autenticação vazia".into()));
    }
    match &auth.kind {
        domain::AuthKind::CookieFile => {
            args.extend(["--cookies".into(), id.into()]);
        }
        domain::AuthKind::BrowserProfile => {
            let browser = id.find([':', '+']).map_or(id, |index| &id[..index]);
            const SUPPORTED: &[&str] = &[
                "brave", "chrome", "chromium", "edge", "firefox", "opera", "safari", "vivaldi",
            ];
            let canonical_browser = browser.to_ascii_lowercase();
            if !SUPPORTED.contains(&canonical_browser.as_str()) {
                return Err(AuthArgsError(format!("navegador não suportado: {browser}")));
            }
            let selector = format!("{canonical_browser}{}", &id[browser.len()..]);
            args.extend(["--cookies-from-browser".into(), selector]);
        }
    }
    Ok(())
}

/// Bitrate solicitado pelo usuário, em kilobits por segundo.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BitrateChoice {
    Auto,
    Kbps(u32),
}

/// Normaliza um bitrate yt-dlp/ffprobe (tbr é expresso em kbps).
pub fn normalize_bitrate_kbps(value: Option<f64>) -> Option<u32> {
    value
        .filter(|v| v.is_finite() && *v > 0.0)
        .map(|v| v.round() as u32)
        .filter(|v| *v > 0)
}

/// Produz o fragmento de seletor yt-dlp para limitar bitrate.
pub fn bitrate_selector(choice: BitrateChoice) -> Option<String> {
    match choice {
        BitrateChoice::Auto => None,
        BitrateChoice::Kbps(value) if value > 0 => Some(format!("[tbr<={value}]")),
        BitrateChoice::Kbps(_) => None,
    }
}

/// Produz opções FFmpeg estruturadas para o stream selecionado.
pub fn ffmpeg_bitrate_args(choice: BitrateChoice, audio: bool) -> Vec<String> {
    match choice {
        BitrateChoice::Auto => Vec::new(),
        BitrateChoice::Kbps(value) if value > 0 => vec![
            if audio { "-b:a" } else { "-b:v" }.into(),
            format!("{value}k"),
        ],
        BitrateChoice::Kbps(_) => Vec::new(),
    }
}

/// Reexporta o modo compartilhado pelo domínio. A disponibilidade é
/// responsabilidade da infraestrutura; este crate só usa um backend quando
/// o chamador o marcou como validado.
pub use downloader_domain::AccelerationMode;

pub fn build_ytdlp_args(url: &str, format: Option<&str>, output_template: &Path) -> CommandArgs {
    build_ytdlp_args_with_playlist(url, format, output_template, false)
}

/// Builds a download command with an explicit playlist policy.
pub fn build_ytdlp_args_with_playlist(
    url: &str,
    format: Option<&str>,
    output_template: &Path,
    allow_playlist: bool,
) -> CommandArgs {
    build_ytdlp_args_with_playlist_and_auth(url, format, output_template, allow_playlist, None)
        .expect("autenticação ausente é sempre válida")
}

/// Builds a download command with an explicit playlist and authentication
/// policy. Authentication options are placed before the URL and remain
/// separate argv entries, so paths with spaces are safe.
pub fn build_ytdlp_args_with_playlist_and_auth(
    url: &str,
    format: Option<&str>,
    output_template: &Path,
    allow_playlist: bool,
    auth: Option<&domain::AuthRef>,
) -> Result<CommandArgs, AuthArgsError> {
    let mut args: Vec<String> = vec![
        "--newline".into(),
        "--progress-template".into(),
        "%(progress._percent_str)s".into(),
    ];
    if !allow_playlist {
        args.insert(0, "--no-playlist".into());
    }
    if let Some(f) = format {
        args.extend(["-f".into(), f.into()]);
    }
    append_auth_args(&mut args, auth)?;
    args.extend([
        "-o".into(),
        output_template.to_string_lossy().into_owned(),
        url.into(),
    ]);
    Ok(CommandArgs {
        program: "yt-dlp".into(),
        args,
    })
}

/// Builds the read-only metadata command used before a download.
pub fn build_analysis_args(url: &str) -> CommandArgs {
    build_analysis_args_with_playlist(url, false)
}

/// Builds a metadata command with an explicit playlist policy.
pub fn build_analysis_args_with_playlist(url: &str, allow_playlist: bool) -> CommandArgs {
    build_analysis_args_with_playlist_and_auth(url, allow_playlist, None)
        .expect("autenticação ausente é sempre válida")
}

/// Builds a read-only metadata command with optional authentication.
pub fn build_analysis_args_with_playlist_and_auth(
    url: &str,
    allow_playlist: bool,
    auth: Option<&domain::AuthRef>,
) -> Result<CommandArgs, AuthArgsError> {
    let mut args = vec!["-J".into(), "--simulate".into()];
    if !allow_playlist {
        args.push("--no-playlist".into());
    } else {
        // Para a prévia de uma playlist, basta obter IDs/títulos/URLs. A
        // extração completa de formatos de cada item torna a consulta muito
        // lenta e só é necessária no download.
        args.extend(["--flat-playlist".into(), "--lazy-playlist".into()]);
    }
    append_auth_args(&mut args, auth)?;
    args.push(url.into());
    Ok(CommandArgs {
        program: "yt-dlp".into(),
        args,
    })
}

/// User-facing format choices mapped to yt-dlp selectors.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FormatChoice {
    Best,
    VideoAndAudio,
    VideoOnly,
    AudioOnly,
}

pub fn format_selector(choice: FormatChoice, max_height: Option<u32>) -> String {
    let height = max_height
        .map(|v| format!("[height<={v}]"))
        .unwrap_or_default();
    match choice {
        FormatChoice::Best | FormatChoice::VideoAndAudio => {
            format!("bestvideo{height}+bestaudio/best")
        }
        FormatChoice::VideoOnly => format!("bestvideo{height}"),
        FormatChoice::AudioOnly => "bestaudio/best".into(),
    }
}

pub fn build_ffmpeg_args(
    input: &Path,
    output: &Path,
    plan: &OutputPlan,
) -> Result<CommandArgs, PlanError> {
    build_ffmpeg_args_with_acceleration(input, output, plan, AccelerationMode::Software, false)
}

/// Alias used by the runtime service for a local, software conversion.
/// It only builds argv; the infrastructure owns process execution and
/// publication of the temporary output.
pub fn build_conversion_args(
    input: &Path,
    output: &Path,
    plan: &OutputPlan,
) -> Result<CommandArgs, PlanError> {
    build_ffmpeg_args(input, output, plan)
}

/// Cria um plano de conversão comum para a interface. O plano seleciona os
/// primeiros streams de vídeo/áudio e escolhe encoders compatíveis com cada
/// contêiner; a execução continua sendo responsabilidade da infraestrutura.
pub fn build_conversion_args_for_format(
    input: &Path,
    output: &Path,
    format: &str,
    mode: AccelerationMode,
    validated_acceleration: bool,
) -> Result<CommandArgs, PlanError> {
    let format = format.trim_start_matches('.').to_ascii_lowercase();
    let (container, video, audio, video_codec, audio_codec, video_action, audio_action) =
        match format.as_str() {
            "mp3" => (
                Container::Mp3,
                None,
                Some(0),
                None,
                Some("libmp3lame".into()),
                None,
                Some(StreamAction::Reencode),
            ),
            "m4a" => (
                Container::M4a,
                None,
                Some(0),
                None,
                Some("aac".into()),
                None,
                Some(StreamAction::Reencode),
            ),
            "flac" => (
                Container::Flac,
                None,
                Some(0),
                None,
                Some("flac".into()),
                None,
                Some(StreamAction::Reencode),
            ),
            "wav" => (
                Container::Wav,
                None,
                Some(0),
                None,
                Some("pcm_s16le".into()),
                None,
                Some(StreamAction::Reencode),
            ),
            "webm" => (
                Container::Webm,
                Some(0),
                Some(1),
                Some("libvpx-vp9".into()),
                Some("libopus".into()),
                Some(StreamAction::Reencode),
                Some(StreamAction::Reencode),
            ),
            "mkv" => (
                Container::Mkv,
                Some(0),
                Some(1),
                None,
                None,
                Some(StreamAction::Copy),
                Some(StreamAction::Copy),
            ),
            _ => (
                Container::Mp4,
                Some(0),
                Some(1),
                Some("libx264".into()),
                Some("aac".into()),
                Some(StreamAction::Reencode),
                Some(StreamAction::Reencode),
            ),
        };
    let plan = OutputPlan {
        container,
        video_codec,
        audio_codec,
        video_action,
        audio_action,
        map_video: video,
        map_audio: audio,
    };
    build_ffmpeg_args_with_acceleration(input, output, &plan, mode, validated_acceleration)
}

/// Builds FFmpeg arguments with an optional, already validated accelerator.
/// Unvalidated hardware requests deliberately fall back to software.
pub fn build_ffmpeg_args_with_acceleration(
    input: &Path,
    output: &Path,
    plan: &OutputPlan,
    mode: AccelerationMode,
    validated: bool,
) -> Result<CommandArgs, PlanError> {
    plan.validate()?;
    let mut args: Vec<String> = vec!["-hide_banner".into(), "-nostdin".into()];
    // `Auto` ainda não foi resolvido pela infraestrutura; tratá-lo como
    // software evita escolher um encoder arbitrário ou anunciar aceleração
    // sem uma sondagem de runtime bem-sucedida.
    let use_acceleration =
        validated && !matches!(mode, AccelerationMode::Software | AccelerationMode::Auto);
    if use_acceleration {
        args.extend(["-hwaccel".into(), acceleration_name(mode).into()]);
    }
    args.extend(["-i".into(), input.to_string_lossy().into_owned()]);
    if let Some(i) = plan.map_video {
        args.extend(["-map".into(), format!("0:{i}")]);
    }
    if let Some(i) = plan.map_audio {
        args.extend(["-map".into(), format!("0:{i}")]);
    }
    if plan.map_video.is_some() {
        args.extend([
            "-c:v".into(),
            plan.video_action
                .map(|a| {
                    if a == StreamAction::Copy {
                        "copy".into()
                    } else {
                        video_encoder(plan.video_codec.as_deref(), mode, use_acceleration)
                    }
                })
                .unwrap_or_else(|| "copy".into()),
        ]);
    }
    if plan.map_audio.is_some() {
        args.extend([
            "-c:a".into(),
            plan.audio_action
                .map(|a| {
                    if a == StreamAction::Copy {
                        "copy".into()
                    } else {
                        plan.audio_codec.clone().unwrap()
                    }
                })
                .unwrap_or_else(|| "copy".into()),
        ]);
    }
    args.extend(["-y".into(), output.to_string_lossy().into_owned()]);
    Ok(CommandArgs {
        program: "ffmpeg".into(),
        args,
    })
}

fn acceleration_name(mode: AccelerationMode) -> &'static str {
    match mode {
        AccelerationMode::Auto => "none",
        AccelerationMode::Vulkan => "vulkan",
        AccelerationMode::Vaapi => "vaapi",
        AccelerationMode::Amf => "amf",
        AccelerationMode::Cuda => "cuda",
        AccelerationMode::Qsv => "qsv",
        AccelerationMode::Software => "none",
    }
}

fn video_encoder(codec: Option<&str>, mode: AccelerationMode, accelerated: bool) -> String {
    let codec = codec.unwrap_or("copy");
    if !accelerated {
        return codec.to_owned();
    }
    match (mode, codec.to_ascii_lowercase().as_str()) {
        (AccelerationMode::Vulkan, "h264" | "libx264") => "h264_vulkan".into(),
        (AccelerationMode::Vulkan, "hevc" | "libx265") => "hevc_vulkan".into(),
        (AccelerationMode::Vulkan, "av1") => "av1_vulkan".into(),
        (AccelerationMode::Vaapi, "h264" | "libx264") => "h264_vaapi".into(),
        (AccelerationMode::Vaapi, "hevc" | "libx265") => "hevc_vaapi".into(),
        (AccelerationMode::Vaapi, "av1") => "av1_vaapi".into(),
        (AccelerationMode::Amf, "h264" | "libx264") => "h264_amf".into(),
        (AccelerationMode::Amf, "hevc" | "libx265") => "hevc_amf".into(),
        (AccelerationMode::Amf, "av1") => "av1_amf".into(),
        (AccelerationMode::Cuda, "h264" | "libx264") => "h264_nvenc".into(),
        (AccelerationMode::Cuda, "hevc" | "libx265") => "hevc_nvenc".into(),
        (AccelerationMode::Qsv, "h264" | "libx264") => "h264_qsv".into(),
        (AccelerationMode::Qsv, "hevc" | "libx265") => "hevc_qsv".into(),
        _ => codec.to_owned(),
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Progress {
    pub percent: Option<f64>,
    pub elapsed_seconds: Option<f64>,
    pub speed: Option<f64>,
    pub eta_seconds: Option<f64>,
}
pub fn parse_progress_line(line: &str) -> Progress {
    let mut p = Progress {
        percent: None,
        elapsed_seconds: None,
        speed: None,
        eta_seconds: None,
    };
    for part in line.split_whitespace() {
        let mut it = part.splitn(2, '=');
        let (k, v) = match (it.next(), it.next()) {
            (Some(k), Some(v)) => (k, v),
            _ => continue,
        };
        let n = v.trim_end_matches('%').trim_end_matches('s').parse().ok();
        match k {
            "percent" | "_percent_str" => p.percent = n,
            "elapsed" => p.elapsed_seconds = n,
            "speed" => p.speed = n,
            "eta" => p.eta_seconds = n,
            _ => {}
        }
    }
    p
}

pub fn temporary_output(dir: &Path, task_id: &str, extension: &str) -> PathBuf {
    dir.join(format!(".downloader-{task_id}.part.{extension}"))
}

#[cfg(test)]
mod tests;
