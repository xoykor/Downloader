//! Executor real da fila. Ele conecta os contratos da aplicação aos
//! processos instalados (`yt-dlp`, `ffprobe` e `ffmpeg`) sem passar por shell.

use crate::{parse_acceleration_mode, ApplicationService, QueueLimits};
use downloader_domain::{
    AccelerationMode, AppCommand, AppError, AuthKind, AuthRef, ErrorCategory, PlanStage, TaskEvent,
    TaskKind, TaskRecord, TaskStatus,
};
use downloader_infra::{
    HardwareBackend, OutputPublisher, ProcessRunner, ProcessSpec, SafeOutputPublisher,
    SqliteTaskRepository, TokioProcessRunner,
};
use downloader_media::{
    build_analysis_args_with_playlist_and_auth, build_ffmpeg_args_with_acceleration,
    build_ytdlp_args_with_playlist_and_auth, format_selector, parse_ffprobe_json, temporary_output,
    Container, FormatChoice, OutputPlan, StreamAction,
};
use std::collections::HashMap;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::time::Duration;
use tokio::sync::mpsc;
use tokio::task::JoinSet;
use tokio_util::sync::CancellationToken;

#[derive(Clone, Debug)]
pub struct EngineConfig {
    pub data_dir: PathBuf,
    pub output_dir: PathBuf,
    /// Aplica o limitador conservador de requisições do YouTube.
    /// Ativado por padrão e alterável pela tela de Configurações.
    pub youtube_protection: bool,
    pub yt_dlp: OsString,
    pub ffmpeg: OsString,
    pub ffprobe: OsString,
}

impl Default for EngineConfig {
    fn default() -> Self {
        let root = std::env::var_os("HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::current_dir().ok())
            .unwrap_or_else(|| PathBuf::from("."));
        let base = std::env::var_os("DOWNLOADER_DATA_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| root.join(".downloader"));
        Self {
            data_dir: base.clone(),
            output_dir: base.join("downloads"),
            youtube_protection: true,
            yt_dlp: OsString::from("yt-dlp"),
            ffmpeg: OsString::from("ffmpeg"),
            ffprobe: OsString::from("ffprobe"),
        }
    }
}

pub struct MediaEngine {
    service: ApplicationService<SqliteTaskRepository>,
    runner: TokioProcessRunner,
    publisher: SafeOutputPublisher,
    config: EngineConfig,
    cancellations: HashMap<String, CancellationToken>,
}

impl MediaEngine {
    pub fn open(config: EngineConfig) -> Result<(Self, Vec<TaskEvent>), AppError> {
        Self::open_with_limits(config, QueueLimits::default())
    }

    pub fn open_with_limits(
        mut config: EngineConfig,
        limits: QueueLimits,
    ) -> Result<(Self, Vec<TaskEvent>), AppError> {
        config.data_dir = expand_home(&config.data_dir);
        config.output_dir = expand_home(&config.output_dir);
        if std::fs::create_dir_all(&config.data_dir).is_err() {
            let fallback = std::env::current_dir()
                .unwrap_or_else(|_| PathBuf::from("."))
                .join(".downloader");
            std::fs::create_dir_all(&fallback)
                .map_err(|error| io_error("abrir aplicação", error))?;
            config.data_dir = fallback;
        }
        if let Err(error) = std::fs::create_dir_all(&config.output_dir) {
            // A preferência padrão pode apontar para um HOME montado como
            // somente leitura (containers/sandboxes). Use o armazenamento
            // local já validado como fallback, preservando destinos explícitos.
            if config.output_dir.starts_with("/home/") {
                config.output_dir = config.data_dir.join("downloads");
                std::fs::create_dir_all(&config.output_dir)
                    .map_err(|_| io_error("criar diretório de saída", error))?;
            } else {
                return Err(io_error("criar diretório de saída", error));
            }
        }
        let repository = match SqliteTaskRepository::open(config.data_dir.join("tasks.sqlite")) {
            Ok(repository) => repository,
            Err(primary_error) => {
                let fallback = std::env::current_dir()
                    .unwrap_or_else(|_| PathBuf::from("."))
                    .join(".downloader");
                std::fs::create_dir_all(&fallback).map_err(|_| {
                    app_error(
                        ErrorCategory::Internal,
                        "persistência",
                        primary_error.to_string(),
                    )
                })?;
                config.data_dir = fallback.clone();
                if config.output_dir.starts_with("/home/") {
                    config.output_dir = fallback.join("downloads");
                    std::fs::create_dir_all(&config.output_dir).map_err(|_| {
                        app_error(
                            ErrorCategory::Internal,
                            "persistência",
                            primary_error.to_string(),
                        )
                    })?;
                }
                SqliteTaskRepository::open(config.data_dir.join("tasks.sqlite")).map_err(
                    |error| {
                        app_error(
                            ErrorCategory::Internal,
                            "persistência",
                            format!("{primary_error}; fallback: {error}"),
                        )
                    },
                )?
            }
        };
        let (service, recovery) = ApplicationService::open(repository, limits)?;
        Ok((
            Self {
                service,
                runner: TokioProcessRunner,
                publisher: SafeOutputPublisher,
                config,
                cancellations: HashMap::new(),
            },
            recovery,
        ))
    }

    /// Executa o loop da aplicação. O canal de comandos é encerrado somente
    /// quando a janela termina; jobs em andamento continuam até terminar.
    pub async fn run(
        mut self,
        mut commands: mpsc::UnboundedReceiver<AppCommand>,
        events: mpsc::UnboundedSender<TaskEvent>,
    ) {
        let mut jobs = JoinSet::new();
        loop {
            tokio::select! {
                command = commands.recv() => {
                    let Some(command) = command else { break; };
                    self.handle_command(command, &events, &mut jobs).await;
                }
                result = jobs.join_next(), if !jobs.is_empty() => {
                    if let Some(result) = result {
                        self.handle_completion(result, &events, &mut jobs).await;
                    }
                }
            }
        }
        while let Some(result) = jobs.join_next().await {
            self.handle_completion(result, &events, &mut jobs).await;
        }
    }

    async fn handle_command(
        &mut self,
        command: AppCommand,
        events: &mpsc::UnboundedSender<TaskEvent>,
        jobs: &mut JoinSet<ExecutionOutcome>,
    ) {
        if let AppCommand::Analyze {
            url,
            auth,
            playlist,
        } = command
        {
            let id = format!("análise-{}", now_ms());
            let cancel = CancellationToken::new();
            self.cancellations.insert(id.clone(), cancel.clone());
            let config = self.config.clone();
            let events = events.clone();
            jobs.spawn(async move {
                execute_analysis(id, url, auth, playlist, config, events, cancel).await
            });
            return;
        }
        if let AppCommand::Cancel { ref task_id } = command {
            if let Some(token) = self.cancellations.get(task_id) {
                token.cancel();
            }
            if task_id.starts_with("análise-") {
                return;
            }
        }
        if let AppCommand::UpdateSettings { ref values } = command {
            if let Some(path) = values.get("diretorio_saida").and_then(|v| v.as_str()) {
                self.config.output_dir = expand_home(Path::new(path));
                let _ = std::fs::create_dir_all(&self.config.output_dir);
            }
            if let Some(enabled) = values.get("youtube_protecao").and_then(|v| v.as_bool()) {
                self.config.youtube_protection = enabled;
            }
        }
        match self.service.dispatch(command) {
            Ok(new_events) => {
                for event in new_events {
                    let _ = events.send(event);
                }
            }
            Err(error) => {
                let _ = events.send(error_event("sistema", error));
            }
        }
        self.start_ready(events, jobs).await;
    }

    async fn start_ready(
        &mut self,
        events: &mpsc::UnboundedSender<TaskEvent>,
        jobs: &mut JoinSet<ExecutionOutcome>,
    ) {
        while let Ok(Some(started)) = self.service.start_next() {
            let Some(task) = self.service.state().task(&started.task_id).cloned() else {
                break;
            };
            let _ = events.send(started.clone());
            let token = CancellationToken::new();
            self.cancellations.insert(task.id.clone(), token.clone());
            let runner = self.runner.clone();
            let publisher = self.publisher.clone();
            let config = self.config.clone();
            let events = events.clone();
            jobs.spawn(async move {
                execute_task(task, config, runner, publisher, token, events).await
            });
        }
    }

    async fn handle_completion(
        &mut self,
        result: Result<ExecutionOutcome, tokio::task::JoinError>,
        events: &mpsc::UnboundedSender<TaskEvent>,
        jobs: &mut JoinSet<ExecutionOutcome>,
    ) {
        let outcome = match result {
            Ok(value) => value,
            Err(error) => ExecutionOutcome {
                task_id: String::new(),
                status: TaskStatus::Falhou,
                error: Some(app_error(
                    ErrorCategory::Internal,
                    "execução",
                    error.to_string(),
                )),
                destination: None,
            },
        };
        if !outcome.task_id.is_empty() {
            self.cancellations.remove(&outcome.task_id);
            if outcome.task_id.starts_with("análise-") {
                self.start_ready(events, jobs).await;
                return;
            }
            if let Some(destination) = outcome.destination.clone() {
                let _ = self
                    .service
                    .set_destination(&outcome.task_id, destination.to_string_lossy().into_owned());
            }
            match self
                .service
                .finish(&outcome.task_id, outcome.status, outcome.error)
            {
                Ok(event) => {
                    let _ = events.send(event);
                }
                Err(error) => {
                    let _ = events.send(error_event("fila", error));
                }
            }
        }
        self.start_ready(events, jobs).await;
    }
}

#[derive(Debug)]
struct ExecutionOutcome {
    task_id: String,
    status: TaskStatus,
    error: Option<AppError>,
    destination: Option<PathBuf>,
}

fn analysis_finished(id: String) -> ExecutionOutcome {
    ExecutionOutcome {
        task_id: id,
        status: TaskStatus::Concluido,
        error: None,
        destination: None,
    }
}
async fn execute_analysis(
    id: String,
    url: String,
    auth: Option<AuthRef>,
    playlist: bool,
    config: EngineConfig,
    events: mpsc::UnboundedSender<TaskEvent>,
    cancel: CancellationToken,
) -> ExecutionOutcome {
    let _ = events.send(TaskEvent {
        task_id: id.clone(),
        sequence: 0,
        status: TaskStatus::Analisando,
        stage: Some(PlanStage::Download),
        progress: None,
        speed: None,
        eta_seconds: None,
        message: Some("Consultando metadados…".into()),
        destination: None,
    });
    let prepared_auth = match prepare_auth(auth.as_ref()) {
        Ok(auth) => auth,
        Err(error) => {
            let _ = events.send(TaskEvent {
                task_id: id.clone(),
                sequence: 1,
                status: TaskStatus::Falhou,
                stage: Some(PlanStage::Validate),
                progress: Some(1.0),
                speed: None,
                eta_seconds: None,
                message: Some(error.message),
                destination: None,
            });
            return analysis_finished(id);
        }
    };
    let command =
        match build_analysis_args_with_playlist_and_auth(&url, playlist, prepared_auth.as_ref()) {
            Ok(command) => command,
            Err(error) => {
                let _ = events.send(TaskEvent {
                    task_id: id.clone(),
                    sequence: 1,
                    status: TaskStatus::Falhou,
                    stage: Some(PlanStage::Validate),
                    progress: Some(1.0),
                    speed: None,
                    eta_seconds: None,
                    message: Some(
                        app_error(ErrorCategory::Authentication, "autenticação", error.0).message,
                    ),
                    destination: None,
                });
                return analysis_finished(id);
            }
        };
    let result = crate::network::run(
        crate::network::Request {
            spec: ProcessSpec {
                executable: config.yt_dlp.clone(),
                args: command.args.into_iter().map(OsString::from).collect(),
                current_dir: None,
                timeout: Some(Duration::from_secs(if playlist { 300 } else { 180 })),
            },
            url: &url,
            data_dir: &config.data_dir,
            cancel: cancel.clone(),
            capture: true,
            protection_enabled: config.youtube_protection,
        },
        |_| {},
        |message, seconds| network_notice(&events, &id, message, seconds, true),
    )
    .await;
    let (status, message) = match result {
        Ok(process) if process.code == Some(0) => {
            let raw = String::from_utf8_lossy(&process.stdout);
            let entries = downloader_media::parse_ytdlp_entries(&raw).unwrap_or_default();
            let message = if entries.len() > 1 {
                format!("Playlist encontrada: {} itens", entries.len())
            } else {
                entries
                    .into_iter()
                    .next()
                    .and_then(|media| media.title)
                    .map(|title| format!("Disponível: {title}"))
                    .unwrap_or_else(|| "Metadados obtidos.".into())
            };
            (TaskStatus::Concluido, message)
        }
        Ok(process) if process.cancelled => (TaskStatus::Cancelado, "Análise cancelada".into()),
        Ok(process) if process.timed_out => (
            TaskStatus::Falhou,
            "Análise expirou; tente novamente.".into(),
        ),
        Ok(process) => (TaskStatus::Falhou, stderr_message(&process.stderr)),
        Err(error) => (TaskStatus::Falhou, error.to_string()),
    };
    let _ = events.send(TaskEvent {
        task_id: id.clone(),
        sequence: 1,
        status,
        stage: Some(PlanStage::Validate),
        progress: Some(1.0),
        speed: None,
        eta_seconds: None,
        message: Some(message),
        destination: None,
    });
    analysis_finished(id)
}
fn network_notice(
    events: &mpsc::UnboundedSender<TaskEvent>,
    id: &str,
    message: &str,
    seconds: Option<u64>,
    analysis: bool,
) {
    let waiting = seconds != Some(0);
    let _ = events.send(TaskEvent {
        task_id: id.into(),
        sequence: 0,
        status: if waiting {
            TaskStatus::NaFila
        } else if analysis {
            TaskStatus::Analisando
        } else {
            TaskStatus::Baixando
        },
        stage: Some(PlanStage::Download),
        progress: None,
        speed: None,
        eta_seconds: seconds,
        message: Some(match seconds {
            Some(n) if n > 0 => format!("{message} · espera de {n} s"),
            _ => message.into(),
        }),
        destination: None,
    });
}

async fn execute_task(
    task: TaskRecord,
    config: EngineConfig,
    runner: TokioProcessRunner,
    publisher: SafeOutputPublisher,
    cancel: CancellationToken,
    events: mpsc::UnboundedSender<TaskEvent>,
) -> ExecutionOutcome {
    let id = task.id.clone();
    let result = match task.kind {
        TaskKind::Download => {
            execute_download(&task, &config, &runner, &publisher, cancel, &events).await
        }
        TaskKind::Convert => execute_convert(&task, &config, &runner, &publisher, cancel).await,
        TaskKind::Validate => execute_validate(&task, &config, &runner, cancel).await,
        TaskKind::Merge => execute_convert(&task, &config, &runner, &publisher, cancel).await,
    };
    match result {
        Ok(destination) => ExecutionOutcome {
            task_id: id,
            status: TaskStatus::Concluido,
            error: None,
            destination,
        },
        Err(error) if error.category == ErrorCategory::Cancelled => ExecutionOutcome {
            task_id: id,
            status: TaskStatus::Cancelado,
            error: None,
            destination: None,
        },
        Err(error) => ExecutionOutcome {
            task_id: id,
            status: TaskStatus::Falhou,
            error: Some(error),
            destination: None,
        },
    }
}

async fn execute_download(
    task: &TaskRecord,
    config: &EngineConfig,
    runner: &TokioProcessRunner,
    publisher: &SafeOutputPublisher,
    cancel: CancellationToken,
    events: &mpsc::UnboundedSender<TaskEvent>,
) -> Result<Option<PathBuf>, AppError> {
    let url = task
        .input_url
        .as_deref()
        .ok_or_else(|| app_error(ErrorCategory::InvalidUrl, "download", "URL ausente"))?;
    if !(url.starts_with("http://") || url.starts_with("https://")) {
        return Err(app_error(
            ErrorCategory::InvalidUrl,
            "download",
            "A URL deve começar com http:// ou https://",
        ));
    }
    let output_dir = output_directory(task.destination.as_deref(), config, "download")?;
    let playlist = task
        .options
        .get("playlist")
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(false);
    let staging = output_dir.join(format!(".downloader-{}", task.id));
    std::fs::create_dir_all(&staging).map_err(|e| io_error("temporários", e))?;
    let template = staging.join("%(title).180B [%(id)s].%(ext)s");
    let requested_format = task
        .options
        .get("format")
        .and_then(|v| v.as_str())
        .or_else(|| {
            task.options
                .get("formato_download")
                .and_then(|v| v.as_str())
        });
    let mode = task
        .options
        .get("mode")
        .and_then(serde_json::Value::as_str)
        .or_else(|| task.options.get("tipo").and_then(serde_json::Value::as_str))
        .unwrap_or("video_audio");
    let quality = task
        .options
        .get("quality")
        .and_then(serde_json::Value::as_str)
        .or_else(|| {
            task.options
                .get("qualidade")
                .and_then(serde_json::Value::as_str)
        })
        .unwrap_or("best");
    let format = requested_format
        .map(str::to_owned)
        .unwrap_or_else(|| download_selector(mode, quality));
    let bitrate = task
        .options
        .get("bitrate_kbps")
        .and_then(serde_json::Value::as_u64)
        .or_else(|| {
            task.options
                .get("bitrate")
                .and_then(serde_json::Value::as_str)
                .and_then(parse_bitrate)
        })
        .filter(|value| (16..=1_000).contains(value));
    let mut selector = bitrate
        .map(|value| apply_audio_bitrate(&format, mode, value))
        .unwrap_or_else(|| format.to_owned());
    let output_format = task
        .options
        .get("output_format")
        .or_else(|| task.options.get("formato_saida"))
        .or_else(|| task.options.get("formato"))
        .and_then(serde_json::Value::as_str)
        .unwrap_or("auto")
        .to_ascii_lowercase();
    if matches!(output_format.as_str(), "mp3" | "opus") {
        selector = "bestaudio/best".into();
    }
    let existing =
        crate::library::existing(&output_dir, &output_format, mode, config, cancel.clone()).await;
    let archive = staging.join("download-archive.txt");
    std::fs::write(
        &archive,
        existing
            .keys()
            .map(|key| format!("{key}\n"))
            .collect::<String>(),
    )
    .map_err(|e| io_error("índice de downloads", e))?;
    let prepared_auth = prepare_auth(task.auth.as_ref())?;
    let command = build_ytdlp_args_with_playlist_and_auth(
        url,
        Some(&selector),
        &template,
        playlist,
        prepared_auth.as_ref(),
    )
    .map_err(|error| app_error(ErrorCategory::Authentication, "autenticação", error.0))?;
    let mut command_args = command.args;
    command_args.extend([
        "--download-archive".into(),
        archive.to_string_lossy().into_owned(),
        "--no-quiet".into(),
        "--no-colors".into(),
    ]);
    if matches!(output_format.as_str(), "mp3" | "opus") {
        command_args.extend(["-x".into(), "--audio-format".into(), output_format.clone()]);
        if let Some(rate) = bitrate {
            command_args.extend(["--audio-quality".into(), format!("{rate}K")]);
        }
    } else if matches!(output_format.as_str(), "mp4" | "mkv" | "webm") {
        command_args.extend(["--merge-output-format".into(), output_format.clone()]);
    }
    if task
        .options
        .get("embed_thumbnail")
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(true)
        && matches!(output_format.as_str(), "opus" | "mp3" | "mp4" | "mkv")
    {
        command_args.extend([
            if output_format == "opus" {
                "--write-thumbnail".into()
            } else {
                "--embed-thumbnail".into()
            },
            "--convert-thumbnails".into(),
            "jpg".into(),
        ]);
    }
    command_args.extend([
        "--no-write-playlist-metafiles".into(),
        "--embed-metadata".into(),
        "--progress".into(),
        "--progress-delta".into(),
        "0.2".into(),
        "--progress-template".into(),
        "download:TRACK %(info.{id,title,playlist_index})j %(progress.{downloaded_bytes,total_bytes,total_bytes_estimate,status})j".into(),
        "--print".into(),
        "after_move:FILE %(.{id,title,playlist_index,filepath,extractor,extractor_key})j".into(),
    ]);
    let (completed_tx, mut completed_rx) = mpsc::unbounded_channel();
    let mut tracks = HashMap::<String, String>::new();
    let mut skipped = 0usize;
    let download = async {
        let result = crate::network::run(
            crate::network::Request {
                spec: ProcessSpec {
                    executable: config.yt_dlp.clone(),
                    args: command_args.iter().cloned().map(OsString::from).collect(),
                    current_dir: None,
                    timeout: None,
                },
                url,
                data_dir: &config.data_dir,
                cancel: cancel.clone(),
                capture: false,
                protection_enabled: config.youtube_protection,
            },
            |line| {
                if line.contains("has already been recorded in the archive") {
                    skipped += 1;
                    let _ = events.send(track_event(
                        format!("{}::skip-{skipped}", task.id),
                        format!(
                            "Já existe — download pulado: {}",
                            line.trim_start_matches("[download] ")
                                .trim_end_matches("has already been recorded in the archive")
                                .trim()
                        ),
                        TaskStatus::Concluido,
                        Some(1.0),
                    ));
                }
                if let Some(raw) = line.strip_prefix("TRACK ") {
                    let mut values =
                        serde_json::Deserializer::from_str(raw).into_iter::<serde_json::Value>();
                    if let (Some(Ok(info)), Some(Ok(progress))) = (values.next(), values.next()) {
                        let key = track_key(task, &info);
                        let title = info["title"].as_str().unwrap_or("Faixa").to_owned();
                        tracks.insert(key.clone(), title.clone());
                        let total = progress["total_bytes"]
                            .as_f64()
                            .or_else(|| progress["total_bytes_estimate"].as_f64());
                        let fraction = total.filter(|v| *v > 0.0).and_then(|v| {
                            progress["downloaded_bytes"]
                                .as_f64()
                                .map(|n| (n / v) as f32)
                        });
                        let _ =
                            events.send(track_event(key, title, TaskStatus::Baixando, fraction));
                    }
                } else if let Some(raw) = line.strip_prefix("FILE ") {
                    if let Ok(info) = serde_json::from_str::<serde_json::Value>(raw) {
                        let _ = events.send(track_event(
                            track_key(task, &info),
                            info["title"].as_str().unwrap_or("Faixa").into(),
                            TaskStatus::Validando,
                            None,
                        ));
                        tracks.remove(&track_key(task, &info));
                        let _ = completed_tx.send(info);
                    }
                }
            },
            |message, seconds| network_notice(events, &task.id, message, seconds, false),
        )
        .await
        .map_err(|error| app_error(ErrorCategory::Dependency, "download", error.to_string()));
        drop(completed_tx);
        result
    };
    let publish = async {
        let mut published_paths = Vec::new();
        while let Some(info) = completed_rx.recv().await {
            let key = track_key(task, &info);
            let title = info["title"].as_str().unwrap_or("Faixa").to_owned();
            let result = async {
                let temporary = PathBuf::from(info["filepath"].as_str().ok_or_else(|| {
                    app_error(
                        ErrorCategory::InvalidMedia,
                        "download",
                        "Caminho final ausente",
                    )
                })?);
                let actual = std::fs::canonicalize(&temporary)
                    .map_err(|e| io_error("validar caminho", e))?;
                let allowed =
                    std::fs::canonicalize(&staging).map_err(|e| io_error("validar caminho", e))?;
                if !actual.starts_with(&allowed) {
                    return Err(app_error(
                        ErrorCategory::InvalidMedia,
                        "download",
                        "Arquivo fora do diretório temporário",
                    ));
                }
                validate_media_file(&temporary, config, runner, cancel.clone()).await?;
                if output_format == "opus" && temporary.with_extension("jpg").is_file() {
                    embed_opus_cover(&temporary, config, runner, cancel.clone()).await?;
                }
                let stem = temporary.file_stem().unwrap().to_string_lossy();
                let id_suffix = format!(" [{}]", info["id"].as_str().unwrap_or(""));
                let title = stem.strip_suffix(&id_suffix).unwrap_or(&stem);
                let destination = output_dir.join(format!(
                    "{}.{}",
                    title,
                    temporary.extension().unwrap().to_string_lossy()
                ));
                let published = if let Some(existing) = crate::library::matching_published(
                    &output_dir,
                    &info,
                    temporary.extension().unwrap(),
                ) {
                    publisher.publish(
                        &temporary,
                        &existing,
                        downloader_infra::CollisionPolicy::Skip,
                    )
                } else {
                    publisher.publish(&temporary, &destination, task.collision.clone().into())
                }
                .map_err(|e| app_error(ErrorCategory::Permission, "publicação", e.to_string()))?;
                crate::library::remember(&output_dir, &published.path, &info)
                    .map_err(|e| io_error("registrar arquivo", e))?;
                let _ = events.send(track_event(
                    track_key(task, &info),
                    info["title"].as_str().unwrap_or("Faixa").into(),
                    TaskStatus::Concluido,
                    Some(1.0),
                ));
                Ok::<PathBuf, AppError>(published.path)
            }
            .await;
            match result {
                Ok(path) => published_paths.push(path),
                Err(error) => {
                    let _ = events.send(track_event(
                        key,
                        format!("{title}: {}", error.message),
                        TaskStatus::Falhou,
                        None,
                    ));
                    cancel.cancel();
                    return Err(error);
                }
            }
        }
        Ok::<Vec<PathBuf>, AppError>(published_paths)
    };
    let (process, published_paths) = tokio::join!(download, publish);
    let published_paths = published_paths?;
    let process = process?;
    if process.cancelled || process.code != Some(0) {
        for (key, title) in &tracks {
            let status = if process.cancelled {
                TaskStatus::Cancelado
            } else {
                TaskStatus::Falhou
            };
            let _ = events.send(track_event(key.clone(), title.clone(), status, None));
        }
    }
    if process.cancelled {
        return Err(app_error(
            ErrorCategory::Cancelled,
            "download",
            "Download cancelado",
        ));
    }
    if process.timed_out {
        return Err(app_error(
            ErrorCategory::Timeout,
            "download",
            "Download expirou",
        ));
    }
    if process.code != Some(0) {
        return Err(app_error(
            ErrorCategory::Network,
            "download",
            stderr_message(&process.stderr),
        ));
    }
    if published_paths.is_empty() && skipped == 0 {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "download",
            "yt-dlp terminou sem produzir arquivo",
        ));
    }
    let _ = std::fs::remove_file(&archive);
    let _ = std::fs::remove_dir(&staging);
    Ok(Some(if playlist || published_paths.is_empty() {
        output_dir
    } else {
        published_paths
            .into_iter()
            .next()
            .expect("arquivo publicado")
    }))
}

fn picture_base64(jpeg: &[u8]) -> String {
    let mut picture = Vec::new();
    for number in [3u32, 10] {
        picture.extend(number.to_be_bytes());
    }
    picture.extend(b"image/jpeg");
    for number in [0u32, 0, 0, 24, 0, jpeg.len() as u32] {
        picture.extend(number.to_be_bytes());
    }
    picture.extend(jpeg);
    const TABLE: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut encoded = String::new();
    for chunk in picture.chunks(3) {
        let value = ((chunk[0] as u32) << 16)
            | ((chunk.get(1).copied().unwrap_or(0) as u32) << 8)
            | chunk.get(2).copied().unwrap_or(0) as u32;
        encoded.push(TABLE[((value >> 18) & 63) as usize] as char);
        encoded.push(TABLE[((value >> 12) & 63) as usize] as char);
        encoded.push(if chunk.len() > 1 {
            TABLE[((value >> 6) & 63) as usize] as char
        } else {
            '='
        });
        encoded.push(if chunk.len() > 2 {
            TABLE[(value & 63) as usize] as char
        } else {
            '='
        });
    }
    encoded
}
async fn embed_opus_cover(
    path: &Path,
    config: &EngineConfig,
    runner: &TokioProcessRunner,
    cancel: CancellationToken,
) -> Result<(), AppError> {
    let jpeg = std::fs::read(path.with_extension("jpg")).map_err(|e| io_error("ler capa", e))?;
    let metadata = path.with_extension("cover.ffmetadata");
    let output = path.with_extension("cover.opus");
    // Export existing tags before adding the standard Ogg cover field.
    let exported = runner
        .run(
            ProcessSpec {
                executable: config.ffmpeg.clone(),
                args: vec![
                    "-v".into(),
                    "error".into(),
                    "-i".into(),
                    path.as_os_str().into(),
                    "-f".into(),
                    "ffmetadata".into(),
                    "-".into(),
                ],
                ..Default::default()
            },
            cancel.clone(),
        )
        .await
        .map_err(|e| app_error(ErrorCategory::Dependency, "capa", e.to_string()))?;
    if exported.code != Some(0) {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "capa",
            stderr_message(&exported.stderr),
        ));
    }
    let mut tags = exported.stdout;
    tags.extend(format!("\nMETADATA_BLOCK_PICTURE={}\n", picture_base64(&jpeg)).as_bytes());
    std::fs::write(&metadata, tags).map_err(|e| io_error("metadados da capa", e))?;
    let result = runner
        .run(
            ProcessSpec {
                executable: config.ffmpeg.clone(),
                args: vec![
                    "-v".into(),
                    "error".into(),
                    "-y".into(),
                    "-i".into(),
                    path.as_os_str().into(),
                    "-f".into(),
                    "ffmetadata".into(),
                    "-i".into(),
                    metadata.as_os_str().into(),
                    "-map".into(),
                    "0:a".into(),
                    "-map_metadata".into(),
                    "1".into(),
                    "-c:a".into(),
                    "copy".into(),
                    output.as_os_str().into(),
                ],
                ..Default::default()
            },
            cancel,
        )
        .await
        .map_err(|e| app_error(ErrorCategory::Dependency, "capa", e.to_string()))?;
    if result.code != Some(0) {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "capa",
            stderr_message(&result.stderr),
        ));
    }
    std::fs::rename(output, path).map_err(|e| io_error("publicar capa", e))?;
    let _ = std::fs::remove_file(metadata);
    let _ = std::fs::remove_file(path.with_extension("jpg"));
    Ok(())
}

fn apply_audio_bitrate(selector: &str, mode: &str, bitrate_kbps: u64) -> String {
    let constraint = format!("[abr<={bitrate_kbps}]");
    if mode == "audio" {
        return format!("bestaudio{constraint}/bestaudio/best");
    }
    if mode == "video" {
        return selector.to_owned();
    }
    selector.replace("bestaudio", &format!("bestaudio{constraint}"))
}

fn download_selector(mode: &str, quality: &str) -> String {
    let height = quality.parse::<u32>().ok();
    match mode {
        "audio" => format_selector(FormatChoice::AudioOnly, None),
        "video" => format_selector(FormatChoice::VideoOnly, height),
        _ => format_selector(FormatChoice::VideoAndAudio, height),
    }
}

fn parse_bitrate(value: &str) -> Option<u64> {
    let normalized = value.trim().to_ascii_lowercase();
    if normalized == "auto" || normalized.is_empty() {
        return None;
    }
    normalized
        .trim_end_matches("kbps")
        .trim_end_matches('k')
        .parse::<u64>()
        .ok()
}

async fn execute_convert(
    task: &TaskRecord,
    config: &EngineConfig,
    runner: &TokioProcessRunner,
    publisher: &SafeOutputPublisher,
    cancel: CancellationToken,
) -> Result<Option<PathBuf>, AppError> {
    let input = task
        .input_path
        .as_deref()
        .map(|value| expand_home(Path::new(value)))
        .ok_or_else(|| {
            app_error(
                ErrorCategory::InvalidMedia,
                "conversão",
                "Arquivo de entrada ausente",
            )
        })?;
    if !input.is_file() {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "conversão",
            format!("Arquivo não encontrado: {}", input.display()),
        ));
    }
    let probe = runner
        .run(
            ProcessSpec {
                executable: config.ffprobe.clone(),
                args: vec![
                    "-v".into(),
                    "error".into(),
                    "-show_streams".into(),
                    "-show_format".into(),
                    "-of".into(),
                    "json".into(),
                    input.clone().into(),
                ],
                current_dir: None,
                timeout: None,
            },
            cancel.clone(),
        )
        .await
        .map_err(|error| app_error(ErrorCategory::Dependency, "análise", error.to_string()))?;
    if probe.cancelled {
        return Err(app_error(
            ErrorCategory::Cancelled,
            "análise",
            "Conversão cancelada",
        ));
    }
    if probe.code != Some(0) {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "análise",
            stderr_message(&probe.stderr),
        ));
    }
    let response =
        parse_ffprobe_json(&String::from_utf8_lossy(&probe.stdout)).map_err(|error| {
            app_error(
                ErrorCategory::InvalidMedia,
                "análise",
                format!("ffprobe inválido: {error:?}"),
            )
        })?;
    let streams = response.to_stream_info();
    let video = streams
        .iter()
        .find(|s| matches!(s.kind, downloader_domain::StreamKind::Video))
        .map(|s| s.index);
    let audio = streams
        .iter()
        .find(|s| matches!(s.kind, downloader_domain::StreamKind::Audio))
        .map(|s| s.index);
    if video.is_none() && audio.is_none() {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "conversão",
            "Nenhum stream de áudio ou vídeo",
        ));
    }
    let format = task
        .options
        .get("formato")
        .or_else(|| task.options.get("format"))
        .and_then(|v| v.as_str())
        .unwrap_or("mp4")
        .trim_start_matches('.')
        .to_ascii_lowercase();
    let (container, vc, ac, va, aa) = conversion_codecs(&format, video, audio);
    let plan = OutputPlan {
        container,
        video_codec: vc,
        audio_codec: ac,
        video_action: va,
        audio_action: aa,
        map_video: video,
        map_audio: audio,
    };
    let requested = parse_acceleration_mode(&task.options)?;
    let (mode, validated) = validated_acceleration(requested, runner, &config.ffmpeg).await;
    let output_dir = output_directory(task.destination.as_deref(), config, "conversão")?;
    let temporary = temporary_output(&output_dir, &task.id, &format);
    let command = build_ffmpeg_args_with_acceleration(&input, &temporary, &plan, mode, validated)
        .map_err(|error| {
        app_error(ErrorCategory::FormatUnavailable, "planejamento", error.0)
    })?;
    let process = runner
        .run(
            ProcessSpec {
                executable: config.ffmpeg.clone(),
                args: command.args.iter().cloned().map(OsString::from).collect(),
                current_dir: None,
                timeout: None,
            },
            cancel.clone(),
        )
        .await
        .map_err(|error| app_error(ErrorCategory::Dependency, "conversão", error.to_string()))?;
    if process.cancelled {
        return Err(app_error(
            ErrorCategory::Cancelled,
            "conversão",
            "Conversão cancelada",
        ));
    }
    if process.timed_out {
        return Err(app_error(
            ErrorCategory::Timeout,
            "conversão",
            "Conversão expirou",
        ));
    }
    if process.code != Some(0) {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "conversão",
            stderr_message(&process.stderr),
        ));
    }
    validate_media_file(&temporary, config, runner, cancel.clone()).await?;
    let stem = input
        .file_stem()
        .and_then(|v| v.to_str())
        .unwrap_or(&task.id);
    let destination = output_dir.join(format!("{stem}.{format}"));
    let published = publisher
        .publish(&temporary, &destination, task.collision.clone().into())
        .map_err(|error| app_error(ErrorCategory::Permission, "publicação", error.to_string()))?;
    Ok(Some(published.path))
}

async fn validate_media_file(
    path: &Path,
    config: &EngineConfig,
    runner: &TokioProcessRunner,
    cancel: CancellationToken,
) -> Result<(), AppError> {
    let result = runner
        .run(
            ProcessSpec {
                executable: config.ffprobe.clone(),
                args: vec![
                    "-v".into(),
                    "error".into(),
                    "-show_streams".into(),
                    "-show_format".into(),
                    "-of".into(),
                    "json".into(),
                    path.to_path_buf().into(),
                ],
                current_dir: None,
                timeout: None,
            },
            cancel,
        )
        .await
        .map_err(|error| app_error(ErrorCategory::Dependency, "validação", error.to_string()))?;
    if result.cancelled {
        return Err(app_error(
            ErrorCategory::Cancelled,
            "validação",
            "Validação cancelada",
        ));
    }
    if result.code != Some(0) {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "validação",
            stderr_message(&result.stderr),
        ));
    }
    let parsed = parse_ffprobe_json(&String::from_utf8_lossy(&result.stdout)).map_err(|error| {
        app_error(
            ErrorCategory::InvalidMedia,
            "validação",
            format!("ffprobe inválido: {error:?}"),
        )
    })?;
    if parsed.streams.is_empty() {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "validação",
            "Arquivo publicado sem streams",
        ));
    }
    Ok(())
}

async fn execute_validate(
    task: &TaskRecord,
    config: &EngineConfig,
    runner: &TokioProcessRunner,
    cancel: CancellationToken,
) -> Result<Option<PathBuf>, AppError> {
    let input = task
        .input_path
        .as_deref()
        .map(|value| expand_home(Path::new(value)))
        .ok_or_else(|| {
            app_error(
                ErrorCategory::InvalidMedia,
                "validação",
                "Arquivo de entrada ausente",
            )
        })?;
    let result = runner
        .run(
            ProcessSpec {
                executable: config.ffprobe.clone(),
                args: vec![
                    "-v".into(),
                    "error".into(),
                    "-of".into(),
                    "null".into(),
                    input.clone().into(),
                ],
                current_dir: None,
                timeout: None,
            },
            cancel,
        )
        .await
        .map_err(|error| app_error(ErrorCategory::Dependency, "validação", error.to_string()))?;
    if result.cancelled {
        return Err(app_error(
            ErrorCategory::Cancelled,
            "validação",
            "Validação cancelada",
        ));
    }
    if result.code != Some(0) {
        return Err(app_error(
            ErrorCategory::InvalidMedia,
            "validação",
            stderr_message(&result.stderr),
        ));
    }
    Ok(Some(input))
}

fn conversion_codecs(
    format: &str,
    video: Option<u32>,
    audio: Option<u32>,
) -> (
    Container,
    Option<String>,
    Option<String>,
    Option<StreamAction>,
    Option<StreamAction>,
) {
    match format {
        "mp3" => (
            Container::Mp3,
            None,
            Some("libmp3lame".into()),
            None,
            audio.map(|_| StreamAction::Reencode),
        ),
        "m4a" => (
            Container::M4a,
            None,
            Some("aac".into()),
            None,
            audio.map(|_| StreamAction::Reencode),
        ),
        "flac" => (
            Container::Flac,
            None,
            Some("flac".into()),
            None,
            audio.map(|_| StreamAction::Reencode),
        ),
        "wav" => (
            Container::Wav,
            None,
            Some("pcm_s16le".into()),
            None,
            audio.map(|_| StreamAction::Reencode),
        ),
        "webm" => (
            Container::Webm,
            video.map(|_| "libvpx-vp9".into()),
            audio.map(|_| "libopus".into()),
            video.map(|_| StreamAction::Reencode),
            audio.map(|_| StreamAction::Reencode),
        ),
        "mkv" => (
            Container::Mkv,
            None,
            None,
            video.map(|_| StreamAction::Copy),
            audio.map(|_| StreamAction::Copy),
        ),
        _ => (
            Container::Mp4,
            video.map(|_| "libx264".into()),
            audio.map(|_| "aac".into()),
            video.map(|_| StreamAction::Reencode),
            audio.map(|_| StreamAction::Reencode),
        ),
    }
}

async fn validated_acceleration(
    mode: AccelerationMode,
    runner: &TokioProcessRunner,
    ffmpeg: &OsString,
) -> (AccelerationMode, bool) {
    let backend = match mode {
        AccelerationMode::Vulkan => Some(HardwareBackend::Vulkan),
        AccelerationMode::Vaapi => Some(HardwareBackend::Vaapi),
        AccelerationMode::Amf => Some(HardwareBackend::Amf),
        AccelerationMode::Cuda => Some(HardwareBackend::Cuda),
        AccelerationMode::Qsv => Some(HardwareBackend::Qsv),
        _ => None,
    };
    let Some(backend) = backend else {
        return (mode, false);
    };
    let probe = downloader_infra::probe_backend(runner, ffmpeg.clone(), backend).await;
    if probe.usable {
        (mode, true)
    } else {
        (AccelerationMode::Software, false)
    }
}

fn track_key(task: &TaskRecord, info: &serde_json::Value) -> String {
    format!(
        "{}::{}::{}",
        task.id,
        info["playlist_index"],
        info["id"].as_str().unwrap_or("item")
    )
}
fn track_event(
    task_id: String,
    title: String,
    status: TaskStatus,
    progress: Option<f32>,
) -> TaskEvent {
    TaskEvent {
        task_id,
        sequence: 0,
        status,
        stage: Some(PlanStage::Download),
        progress,
        speed: None,
        eta_seconds: None,
        message: Some(title),
        destination: None,
    }
}
fn expand_home(path: &Path) -> PathBuf {
    let value = path.to_string_lossy();
    if let Some(rest) = value.strip_prefix("~/") {
        if let Some(home) = std::env::var_os("HOME") {
            return PathBuf::from(home).join(rest);
        }
    }
    path.to_path_buf()
}

/// Resolve a task's authentication reference immediately before spawning
/// `yt-dlp`. Cookie files are checked as Netscape files and `~` is expanded;
/// browser profiles remain references understood by `yt-dlp`. The cookie
/// contents are never copied into task state, arguments beyond the file path,
/// events, or errors.
fn prepare_auth(auth: Option<&AuthRef>) -> Result<Option<AuthRef>, AppError> {
    let Some(auth) = auth else {
        return Ok(None);
    };
    let mut prepared = auth.clone();
    match &auth.kind {
        AuthKind::CookieFile => {
            let path = expand_home(Path::new(auth.id.trim()));
            downloader_infra::validate_netscape_file(&path).map_err(|error| {
                app_error(
                    ErrorCategory::Authentication,
                    "autenticação",
                    error.to_string(),
                )
            })?;
            prepared.id = path.to_string_lossy().into_owned();
        }
        AuthKind::BrowserProfile => {
            if auth.id.trim().eq_ignore_ascii_case("auto") {
                let Some(browser) = crate::detect_browser_profile() else {
                    // Nenhum perfil detectável: o download segue anônimo.
                    return Ok(None);
                };
                prepared.id = browser;
            } else {
                prepared.id = auth.id.trim().to_owned();
            }
        }
    }
    Ok(Some(prepared))
}

fn output_directory(
    requested: Option<&str>,
    config: &EngineConfig,
    step: &str,
) -> Result<PathBuf, AppError> {
    let path = requested
        .map(|value| expand_home(Path::new(value)))
        .unwrap_or_else(|| config.output_dir.clone());
    match std::fs::create_dir_all(&path) {
        Ok(()) => Ok(path),
        Err(error) if requested.is_some_and(|value| value.starts_with("~")) => {
            std::fs::create_dir_all(&config.output_dir)
                .map(|_| config.output_dir.clone())
                .map_err(|_| io_error(step, error))
        }
        Err(error) => Err(io_error(step, error)),
    }
}
fn stderr_message(bytes: &[u8]) -> String {
    let value = String::from_utf8_lossy(bytes);
    value
        .lines()
        .rfind(|line| !line.trim().is_empty())
        .unwrap_or("processo terminou com erro")
        .trim()
        .to_owned()
}
fn app_error(category: ErrorCategory, step: &str, message: impl Into<String>) -> AppError {
    AppError {
        category,
        message: message.into(),
        step: Some(step.into()),
        code: None,
    }
}
fn io_error(step: &str, error: std::io::Error) -> AppError {
    app_error(ErrorCategory::Permission, step, error.to_string())
}
fn error_event(step: &str, error: AppError) -> TaskEvent {
    TaskEvent {
        task_id: "sistema".into(),
        sequence: 0,
        status: TaskStatus::Falhou,
        stage: Some(PlanStage::Validate),
        progress: None,
        speed: None,
        eta_seconds: None,
        message: Some(format!("{step}: {}", error.message)),
        destination: None,
    }
}

fn now_ms() -> u128 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |duration| duration.as_millis())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    #[tokio::test]
    async fn publishes_cover_before_playlist_finishes_even_if_next_item_fails() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        let staging = dir.path().join(".downloader-test");
        std::fs::create_dir(&staging).unwrap();
        let audio = staging.join("Song [id].opus");
        assert!(Command::new("ffmpeg")
            .args([
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "sine=duration=0.2",
                "-c:a",
                "libopus"
            ])
            .arg(&audio)
            .status()
            .unwrap()
            .success());
        assert!(Command::new("ffmpeg")
            .args([
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "color=red:s=64x64",
                "-frames:v",
                "1"
            ])
            .arg(audio.with_extension("jpg"))
            .status()
            .unwrap()
            .success());
        let info = serde_json::json!({"id":"id", "title":"Song", "filepath":audio});
        let script = dir.path().join("extractor");
        let final_path = dir.path().join("Song.opus");
        std::fs::write(&script, format!("#!/bin/sh\nprintf '%s\\n' 'FILE {info}'\nwhile [ ! -f '{}' ]; do sleep 0.05; done\nexit 1\n", final_path.display())).unwrap();
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755)).unwrap();
        let task = TaskRecord {
            id: "test".into(),
            kind: TaskKind::Download,
            input_url: Some("https://example.test/list".into()),
            input_path: None,
            options: serde_json::json!({"playlist":true,"output_format":"opus"}),
            destination: Some(dir.path().to_string_lossy().into_owned()),
            collision: downloader_domain::CollisionPolicy::Rename,
            auth: None,
            status: TaskStatus::NaFila,
            temporary_path: None,
            error: None,
            created_at_ms: 0,
            updated_at_ms: 0,
        };
        let config = EngineConfig {
            yt_dlp: script.into(),
            ..EngineConfig::default()
        };
        let (tx, mut rx) = mpsc::unbounded_channel();
        let result = tokio::time::timeout(
            Duration::from_secs(5),
            execute_download(
                &task,
                &config,
                &TokioProcessRunner,
                &SafeOutputPublisher,
                CancellationToken::new(),
                &tx,
            ),
        )
        .await
        .expect("publication must not wait for playlist exit");
        assert!(result.is_err());
        assert!(final_path.is_file());
        assert!(!audio.with_extension("jpg").exists());
        let mut done = false;
        while let Ok(event) = rx.try_recv() {
            if event.status == TaskStatus::Concluido {
                done = true;
            }
        }
        assert!(done);
    }

    #[tokio::test]
    async fn opus_cover_is_readable_as_attached_picture() {
        let dir = tempfile::tempdir().unwrap();
        let audio = dir.path().join("Título.opus");
        assert!(Command::new("ffmpeg")
            .args([
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "sine=duration=0.2",
                "-c:a",
                "libopus"
            ])
            .arg(&audio)
            .status()
            .unwrap()
            .success());
        assert!(Command::new("ffmpeg")
            .args([
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "color=red:s=64x64",
                "-frames:v",
                "1"
            ])
            .arg(audio.with_extension("jpg"))
            .status()
            .unwrap()
            .success());
        embed_opus_cover(
            &audio,
            &EngineConfig::default(),
            &TokioProcessRunner,
            CancellationToken::new(),
        )
        .await
        .unwrap();
        let result = Command::new("ffprobe")
            .args(["-v", "error", "-show_streams", "-of", "json"])
            .arg(audio)
            .output()
            .unwrap();
        let info: serde_json::Value = serde_json::from_slice(&result.stdout).unwrap();
        let streams = info["streams"].as_array().unwrap();
        assert!(streams.iter().any(|s| s["codec_name"] == "opus"));
        assert!(streams
            .iter()
            .any(|s| s["codec_name"] == "mjpeg" && s["disposition"]["attached_pic"] == 1));
    }

    #[test]
    fn cookie_auth_is_validated_without_persisting_cookie_contents() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("cookies.txt");
        std::fs::write(
            &path,
            "# Netscape HTTP Cookie File\n.example.test\tTRUE\t/\tTRUE\t0\tsid\tsecret\n",
        )
        .expect("cookies");
        let auth = AuthRef {
            id: path.to_string_lossy().into_owned(),
            kind: AuthKind::CookieFile,
        };
        let prepared = prepare_auth(Some(&auth))
            .expect("valid cookie file")
            .unwrap();
        assert_eq!(prepared.kind, AuthKind::CookieFile);
        assert_eq!(prepared.id, path.to_string_lossy());
        assert!(!serde_json::to_string(&prepared)
            .expect("serialize auth")
            .contains("secret"));
    }

    #[test]
    fn missing_cookie_file_is_an_authentication_error() {
        let auth = AuthRef {
            id: "/definitely/missing.cookies".into(),
            kind: AuthKind::CookieFile,
        };
        let error = prepare_auth(Some(&auth)).expect_err("missing cookies must fail");
        assert_eq!(error.category, ErrorCategory::Authentication);
    }

    #[tokio::test]
    async fn converts_a_local_media_file_and_publishes_output() {
        let dir = tempfile::tempdir().expect("tempdir");
        let input = dir.path().join("entrada.mkv");
        let output = dir.path().join("saída");
        let generated = Command::new("ffmpeg")
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "testsrc=size=160x90:rate=5",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:sample_rate=8000",
                "-t",
                "0.4",
                "-c:v",
                "libx264",
                "-c:a",
                "aac",
                input.to_str().expect("input"),
                "-y",
            ])
            .status()
            .expect("ffmpeg instalado");
        assert!(generated.success());
        let config = EngineConfig {
            data_dir: dir.path().join("state"),
            output_dir: output.clone(),
            ..EngineConfig::default()
        };
        let (engine, recovery) =
            MediaEngine::open_with_limits(config, QueueLimits::default()).expect("open");
        assert!(recovery.is_empty());
        let (command_tx, command_rx) = mpsc::unbounded_channel();
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        command_tx
            .send(AppCommand::Enqueue(Box::new(TaskRecord {
                id: "local-1".into(),
                kind: TaskKind::Convert,
                input_url: None,
                input_path: Some(input.to_string_lossy().into_owned()),
                options: serde_json::json!({"formato":"mp4", "aceleracao":"software"}),
                destination: Some(output.to_string_lossy().into_owned()),
                collision: downloader_domain::CollisionPolicy::Rename,
                auth: None,
                status: TaskStatus::NaFila,
                temporary_path: None,
                error: None,
                created_at_ms: 0,
                updated_at_ms: 0,
            })))
            .expect("enqueue");
        drop(command_tx);
        engine.run(command_rx, event_tx).await;
        let mut statuses = Vec::new();
        while let Ok(event) = event_rx.try_recv() {
            statuses.push(event.status);
        }
        assert!(
            statuses.contains(&TaskStatus::Concluido),
            "eventos: {statuses:?}"
        );
        assert!(output.join("entrada.mp4").is_file());
    }

    #[test]
    fn bitrate_is_applied_only_to_audio_selection() {
        assert_eq!(
            apply_audio_bitrate("bestvideo*+bestaudio/best", "video_audio", 128),
            "bestvideo*+bestaudio[abr<=128]/best"
        );
        assert_eq!(
            apply_audio_bitrate("bestvideo*+bestaudio/best", "audio", 320),
            "bestaudio[abr<=320]/bestaudio/best"
        );
        assert_eq!(
            apply_audio_bitrate("bestvideo[height<=720]", "video", 64),
            "bestvideo[height<=720]"
        );
    }
}
