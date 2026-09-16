use downloader_application::{EngineConfig, MediaEngine};
use downloader_domain::{
    AppCommand, AuthKind, AuthRef, CollisionPolicy, TaskKind, TaskRecord, TaskStatus,
};
use std::path::PathBuf;
use tokio::sync::mpsc;

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        eprintln!("erro: {}", error.message);
        std::process::exit(1);
    }
}

async fn run() -> Result<(), downloader_domain::AppError> {
    let mut args = std::env::args().skip(1);
    let command = args.next().unwrap_or_else(|| "help".into());
    if matches!(command.as_str(), "help" | "--help" | "-h") {
        print_help();
        return Ok(());
    }
    let mut config = EngineConfig::default();
    let task = match command.as_str() {
        "analyze" | "analisar" => {
            let url = args.next().ok_or_else(|| usage("URL ausente"))?;
            let (auth, playlist) = parse_auth_flags(&mut args)?;
            let (engine, recovery) = MediaEngine::open(config)?;
            return run_engine(
                engine,
                recovery,
                AppCommand::Analyze {
                    url,
                    auth,
                    playlist,
                },
            )
            .await;
        }
        "download" | "baixar" => {
            let url = args.next().ok_or_else(|| usage("URL ausente"))?;
            let mut destination = None;
            let mut playlist = false;
            let mut output_format = "auto".to_owned();
            let mut bitrate = "auto".to_owned();
            let mut auth = None;
            while let Some(value) = args.next() {
                match value.as_str() {
                    "--format" => output_format = required_flag_value(&mut args, "--format")?,
                    "--bitrate" => bitrate = required_flag_value(&mut args, "--bitrate")?,
                    "--playlist" | "--lista" => playlist = true,
                    "--cookies" | "--cookie-file" => {
                        auth = Some(AuthRef {
                            id: required_flag_value(&mut args, "--cookies")?,
                            kind: AuthKind::CookieFile,
                        });
                    }
                    "--browser" | "--cookies-from-browser" => {
                        auth = Some(AuthRef {
                            id: required_flag_value(&mut args, "--browser")?,
                            kind: AuthKind::BrowserProfile,
                        });
                    }
                    _ if destination.is_none() => destination = Some(PathBuf::from(value)),
                    _ => return Err(usage("opção de download desconhecida")),
                }
            }
            if let Some(path) = &destination {
                config.output_dir = path.clone();
            }
            task(
                "download",
                TaskKind::Download,
                Some(url),
                None,
                serde_json::json!({"playlist": playlist, "output_format": output_format, "bitrate": bitrate}),
                destination,
                auth,
            )
        }
        "convert" | "converter" => {
            let input = args.next().ok_or_else(|| usage("arquivo ausente"))?;
            let format = args.next().unwrap_or_else(|| "mp4".into());
            let destination = args.next().map(PathBuf::from);
            if let Some(path) = &destination {
                config.output_dir = path.clone();
            }
            task(
                "convert",
                TaskKind::Convert,
                None,
                Some(input),
                serde_json::json!({"formato": format, "aceleracao": "auto"}),
                destination,
                None,
            )
        }
        _ => return Err(usage("comando desconhecido")),
    };
    let (engine, recovery) = MediaEngine::open(config)?;
    run_engine(engine, recovery, AppCommand::Enqueue(Box::new(task))).await
}

fn task(
    id_prefix: &str,
    kind: TaskKind,
    url: Option<String>,
    path: Option<String>,
    options: serde_json::Value,
    destination: Option<PathBuf>,
    auth: Option<AuthRef>,
) -> TaskRecord {
    let id = format!(
        "{id_prefix}-{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_millis())
    );
    TaskRecord {
        id,
        kind,
        input_url: url,
        input_path: path,
        options,
        destination: destination.map(|v| v.to_string_lossy().into_owned()),
        collision: CollisionPolicy::Rename,
        auth,
        status: TaskStatus::NaFila,
        temporary_path: None,
        error: None,
        created_at_ms: 0,
        updated_at_ms: 0,
    }
}

async fn run_engine(
    engine: MediaEngine,
    recovery: Vec<downloader_domain::TaskEvent>,
    command: AppCommand,
) -> Result<(), downloader_domain::AppError> {
    let (command_tx, command_rx) = mpsc::unbounded_channel();
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let worker = tokio::spawn(engine.run(command_rx, event_tx));
    command_tx
        .send(command)
        .map_err(|_| usage("runtime encerrado"))?;
    drop(command_tx);
    for event in recovery {
        print_event(&event);
    }
    let mut failed = None;
    while let Some(event) = event_rx.recv().await {
        print_event(&event);
        if event.status == TaskStatus::Falhou {
            failed = event.message;
        }
    }
    let _ = worker.await;
    failed.map_or(Ok(()), |message| Err(usage(&message)))
}

fn print_event(event: &downloader_domain::TaskEvent) {
    if let Some(progress) = event.progress {
        print!("{:.0}% ", progress * 100.0);
    }
    let status = format!("{:?}", event.status);
    let message = event.message.as_deref().unwrap_or("");
    if let Some(path) = &event.destination {
        println!("{}: {} — {} ({})", event.task_id, status, message, path);
    } else {
        println!("{}: {} — {}", event.task_id, status, message);
    }
}

fn usage(message: &str) -> downloader_domain::AppError {
    downloader_domain::AppError { category: downloader_domain::ErrorCategory::Internal, message: format!("{message}. Use: downloader-cli analyze <url> [--playlist] [--cookies arquivo | --browser navegador[:perfil]] | download <url> [diretório] [--playlist] [--cookies arquivo | --browser navegador[:perfil]] | convert <arquivo> [formato] [diretório]"), step: Some("CLI".into()), code: None }
}

fn print_help() {
    println!("downloader-cli\n\n  analyze <url> [--playlist]\n           [--cookies arquivo | --browser navegador[:perfil]]\n                                             analisa metadados\n  download <url> [diretório] [--playlist]\n           [--cookies arquivo | --browser navegador[:perfil]]\n                                             baixa e publica o arquivo\n  convert <arquivo> [formato] [diretório]    converte localmente (mp4/mkv/mp3/webm)\n\n  --format opus|mp3|mp4|mkv|webm|auto         formato do download\n  --bitrate 128k                             qualidade de áudio\n  --cookies arquivo                          usa cookies.txt Netscape\n  --browser navegador[:perfil]               importa cookies do perfil local\n");
}

fn parse_auth_flags<I>(args: &mut I) -> Result<(Option<AuthRef>, bool), downloader_domain::AppError>
where
    I: Iterator<Item = String>,
{
    let mut auth = None;
    let mut playlist = false;
    while let Some(flag) = args.next() {
        if flag == "--playlist" || flag == "--lista" {
            playlist = true;
            continue;
        }
        let (kind, label) = match flag.as_str() {
            "--cookies" | "--cookie-file" => (AuthKind::CookieFile, "--cookies"),
            "--browser" | "--cookies-from-browser" => (AuthKind::BrowserProfile, "--browser"),
            _ => return Err(usage("opção de análise desconhecida")),
        };
        if auth.is_some() {
            return Err(usage("informe somente uma fonte de cookies"));
        }
        auth = Some(AuthRef {
            id: required_flag_value(args, label)?,
            kind,
        });
    }
    Ok((auth, playlist))
}

fn required_flag_value<I>(args: &mut I, flag: &str) -> Result<String, downloader_domain::AppError>
where
    I: Iterator<Item = String>,
{
    args.next()
        .filter(|value| !value.starts_with('-'))
        .ok_or_else(|| usage(&format!("valor ausente para {flag}")))
}
