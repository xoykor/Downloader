use async_trait::async_trait;
use std::ffi::OsString;
use std::path::PathBuf;
use std::time::Duration;
use thiserror::Error;
use tokio::process::Command;
use tokio_util::sync::CancellationToken;

#[derive(Clone, Debug, Default)]
pub struct ProcessSpec {
    pub executable: OsString,
    pub args: Vec<OsString>,
    pub current_dir: Option<PathBuf>,
    pub timeout: Option<Duration>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProcessResult {
    pub code: Option<i32>,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
    pub timed_out: bool,
    pub cancelled: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExecutableInfo {
    pub name: OsString,
    pub path: PathBuf,
    pub version: Option<String>,
}

#[derive(Debug, Error)]
pub enum ProcessError {
    #[error("não foi possível iniciar o processo: {0}")]
    Spawn(#[source] std::io::Error),
    #[error("falha ao aguardar o processo: {0}")]
    Wait(#[source] std::io::Error),
}

#[async_trait]
pub trait ProcessRunner: Send + Sync {
    async fn run(
        &self,
        spec: ProcessSpec,
        cancel: CancellationToken,
    ) -> Result<ProcessResult, ProcessError>;
    async fn detect(&self, executable: OsString) -> Option<ExecutableInfo>;
}

#[derive(Clone, Debug, Default)]
pub struct TokioProcessRunner;

impl TokioProcessRunner {
    /// Drain both pipes concurrently and deliver stdout records immediately.
    pub async fn run_lines<F: FnMut(&str) + Send>(
        &self,
        spec: ProcessSpec,
        cancel: CancellationToken,
        mut on_line: F,
    ) -> Result<ProcessResult, ProcessError> {
        use tokio::io::{AsyncBufReadExt, AsyncReadExt, BufReader};
        let mut command = Command::new(&spec.executable);
        command
            .args(&spec.args)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .kill_on_drop(true);
        if let Some(dir) = spec.current_dir {
            command.current_dir(dir);
        }
        let mut child = command.spawn().map_err(ProcessError::Spawn)?;
        let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
        let mut stderr = child.stderr.take().unwrap();
        let mut result = ProcessResult::default();
        let work = async {
            let output = async {
                while let Some(line) = lines.next_line().await? {
                    on_line(&line);
                }
                Ok::<_, std::io::Error>(())
            };
            let errors = async { stderr.read_to_end(&mut result.stderr).await };
            let (out, err) = tokio::join!(output, errors);
            out.map_err(ProcessError::Wait)?;
            err.map_err(ProcessError::Wait)?;
            result.code = child.wait().await.map_err(ProcessError::Wait)?.code();
            Ok::<_, ProcessError>(())
        };
        let timeout = spec.timeout;
        tokio::select! {
            _ = cancel.cancelled() => { result.cancelled = true; }
            _ = async { match timeout { Some(duration) => tokio::time::sleep(duration).await, None => std::future::pending::<()>().await } } => { result.timed_out = true; }
            outcome = work => { outcome?; }
        }
        Ok(result)
    }
}

#[async_trait]
impl ProcessRunner for TokioProcessRunner {
    async fn run(
        &self,
        spec: ProcessSpec,
        cancel: CancellationToken,
    ) -> Result<ProcessResult, ProcessError> {
        let mut command = Command::new(&spec.executable);
        // `output()` owns the child future. If timeout/cancelamento vence o
        // select, a future é descartada; kill_on_drop evita deixar o processo
        // externo órfão nesse caminho.
        command
            .args(&spec.args)
            .stdin(std::process::Stdio::null())
            .kill_on_drop(true);
        if let Some(dir) = spec.current_dir {
            command.current_dir(dir);
        }
        let child = command.output();
        let output = if let Some(limit) = spec.timeout {
            tokio::select! {
                _ = cancel.cancelled() => return Ok(ProcessResult { cancelled: true, ..Default::default() }),
                result = tokio::time::timeout(limit, child) => match result {
                    Ok(result) => result.map_err(ProcessError::Wait)?,
                    Err(_) => return Ok(ProcessResult { timed_out: true, ..Default::default() }),
                }
            }
        } else {
            tokio::select! {
                _ = cancel.cancelled() => return Ok(ProcessResult { cancelled: true, ..Default::default() }),
                result = child => result.map_err(ProcessError::Wait)?,
            }
        };
        Ok(ProcessResult {
            code: output.status.code(),
            stdout: output.stdout,
            stderr: output.stderr,
            ..Default::default()
        })
    }

    async fn detect(&self, executable: OsString) -> Option<ExecutableInfo> {
        let requested = executable;
        let path = find_executable(&requested)?;
        let result = self
            .run(
                ProcessSpec {
                    executable: path.clone().into(),
                    args: vec![OsString::from("--version")],
                    timeout: Some(Duration::from_secs(3)),
                    ..Default::default()
                },
                CancellationToken::new(),
            )
            .await
            .ok()?;
        let text = first_line(&result.stdout).or_else(|| first_line(&result.stderr));
        Some(ExecutableInfo {
            name: requested,
            path,
            version: text,
        })
    }
}

fn first_line(bytes: &[u8]) -> Option<String> {
    String::from_utf8_lossy(bytes)
        .lines()
        .map(str::trim)
        .find(|s| !s.is_empty())
        .map(ToOwned::to_owned)
}

fn find_executable(name: &std::ffi::OsStr) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        let candidate = dir.join(name);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn streams_records_before_process_finishes_and_cancels() {
        let cancel = CancellationToken::new();
        let trigger = cancel.clone();
        let mut records = Vec::new();
        let result = TokioProcessRunner
            .run_lines(
                ProcessSpec {
                    executable: "sh".into(),
                    args: vec!["-c".into(), "printf 'first\\n'; exec sleep 10".into()],
                    ..Default::default()
                },
                cancel,
                |line| {
                    records.push(line.to_owned());
                    trigger.cancel();
                },
            )
            .await
            .unwrap();
        assert_eq!(records, ["first"]);
        assert!(result.cancelled);
    }
    #[tokio::test]
    async fn streamed_process_respects_timeout() {
        let result = TokioProcessRunner
            .run_lines(
                ProcessSpec {
                    executable: "sleep".into(),
                    args: vec!["10".into()],
                    timeout: Some(Duration::from_millis(10)),
                    current_dir: None,
                },
                CancellationToken::new(),
                |_| {},
            )
            .await
            .unwrap();
        assert!(result.timed_out);
    }
    #[tokio::test]
    async fn captures_output_without_shell() {
        let r = TokioProcessRunner
            .run(
                ProcessSpec {
                    executable: "printf".into(),
                    args: vec!["ok".into()],
                    ..Default::default()
                },
                CancellationToken::new(),
            )
            .await
            .unwrap();
        assert_eq!(r.code, Some(0));
        assert_eq!(r.stdout, b"ok");
        assert!(!r.timed_out);
    }
    #[tokio::test]
    async fn reports_timeout_and_cancellation() {
        let r = TokioProcessRunner
            .run(
                ProcessSpec {
                    executable: "sleep".into(),
                    args: vec!["2".into()],
                    timeout: Some(Duration::from_millis(5)),
                    ..Default::default()
                },
                CancellationToken::new(),
            )
            .await
            .unwrap();
        assert!(r.timed_out);
        let token = CancellationToken::new();
        token.cancel();
        let r = TokioProcessRunner
            .run(
                ProcessSpec {
                    executable: "sleep".into(),
                    args: vec!["2".into()],
                    ..Default::default()
                },
                token,
            )
            .await
            .unwrap();
        assert!(r.cancelled);
    }
}
