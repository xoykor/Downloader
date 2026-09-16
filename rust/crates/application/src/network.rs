//! Conservative YouTube pacing. Published API quotas do not apply to yt-dlp.
use downloader_infra::{
    ProcessError, ProcessResult, ProcessRunner, ProcessSpec, TokioProcessRunner,
};
use std::{
    fs::{File, OpenOptions, TryLockError},
    path::{Path, PathBuf},
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tokio_util::sync::CancellationToken;

pub(crate) fn is_youtube(url: &str) -> bool {
    let Some((scheme, rest)) = url.split_once("://") else {
        return false;
    };
    if !matches!(scheme, "https" | "http") {
        return false;
    }
    let host = rest
        .split(['/', '?', '#'])
        .next()
        .unwrap_or("")
        .split(':')
        .next()
        .unwrap_or("")
        .to_ascii_lowercase();
    matches!(
        host.as_str(),
        "youtube.com"
            | "www.youtube.com"
            | "m.youtube.com"
            | "music.youtube.com"
            | "youtu.be"
            | "youtube-nocookie.com"
            | "www.youtube-nocookie.com"
    )
}

pub(crate) struct Request<'a> {
    pub spec: ProcessSpec,
    pub url: &'a str,
    pub data_dir: &'a Path,
    pub cancel: CancellationToken,
    pub capture: bool,
    /// Quando falso, o yt-dlp é executado sem o limitador específico do YouTube.
    pub protection_enabled: bool,
}

#[derive(Clone, Copy)]
struct Timing {
    gap: Duration,
    fallback: Duration,
    retries: u32,
    max_videos: usize,
    window: Duration,
}
const TIMING: Timing = Timing {
    gap: Duration::from_secs(5),
    fallback: Duration::from_secs(90),
    retries: 3,
    max_videos: 300,
    window: Duration::from_secs(90 * 60),
};

fn policy_args(spec: &mut ProcessSpec) {
    // Let Rust own retries so 429 pauses the whole playlist instead of advancing.
    for arg in [
        "--ignore-config",
        "--socket-timeout",
        "30",
        "--sleep-requests",
        "5.4",
        "--sleep-interval",
        "5",
        "--max-sleep-interval",
        "5",
        "--concurrent-fragments",
        "1",
        "--retries",
        "0",
        "--fragment-retries",
        "0",
        "--extractor-retries",
        "0",
        "--abort-on-error",
        "--abort-on-unavailable-fragments",
        "--lazy-playlist",
    ] {
        spec.args.push(arg.into());
    }
}

fn epoch_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .min(u64::MAX as u128) as u64
}

struct Gate {
    _lock: File,
    state: PathBuf,
    next_ms: u64,
    videos: Vec<u64>,
}
impl Gate {
    async fn acquire(
        dir: &Path,
        cancel: &CancellationToken,
        notice: &mut impl FnMut(&str, Option<u64>),
    ) -> Result<Option<Self>, ProcessError> {
        std::fs::create_dir_all(dir).map_err(ProcessError::Wait)?;
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(dir.join("youtube-network.lock"))
            .map_err(ProcessError::Wait)?;
        let mut announced = false;
        loop {
            match lock.try_lock() {
                Ok(()) => break,
                Err(TryLockError::WouldBlock) => {
                    if !announced {
                        notice("Aguardando outra operação do YouTube terminar", None);
                        announced = true;
                    }
                    if !sleep(Duration::from_millis(200), cancel).await {
                        return Ok(None);
                    }
                }
                Err(TryLockError::Error(error)) => return Err(ProcessError::Wait(error)),
            }
        }
        let state = dir.join("youtube-network.json");
        let saved = match std::fs::read(&state) {
            Ok(bytes) => serde_json::from_slice::<serde_json::Value>(&bytes).map_err(|e| {
                ProcessError::Wait(std::io::Error::new(std::io::ErrorKind::InvalidData, e))
            })?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                serde_json::json!({"next_ms": 0, "videos": []})
            }
            Err(e) => return Err(ProcessError::Wait(e)),
        };
        let (next_ms, videos) = if let Some(old) = saved.as_u64() {
            (old, Vec::new())
        } else {
            let invalid = || {
                ProcessError::Wait(std::io::Error::new(
                    std::io::ErrorKind::InvalidData,
                    "Estado de limite do YouTube inválido",
                ))
            };
            let next = saved["next_ms"].as_u64().ok_or_else(invalid)?;
            let videos = saved["videos"]
                .as_array()
                .ok_or_else(invalid)?
                .iter()
                .map(|v| v.as_u64().ok_or_else(invalid))
                .collect::<Result<Vec<_>, _>>()?;
            (next, videos)
        };
        Ok(Some(Self {
            _lock: lock,
            state,
            next_ms,
            videos,
        }))
    }
    fn postpone(&mut self, delay: Duration) -> Result<(), ProcessError> {
        self.next_ms = self
            .next_ms
            .max(epoch_ms().saturating_add(delay.as_millis().min(u64::MAX as u128) as u64));
        self.save()
    }
    fn save(&self) -> Result<(), ProcessError> {
        let temporary = self.state.with_extension("tmp");
        let bytes =
            serde_json::to_vec(&serde_json::json!({"next_ms":self.next_ms,"videos":self.videos}))
                .map_err(|e| ProcessError::Wait(std::io::Error::other(e)))?;
        std::fs::write(&temporary, bytes).map_err(ProcessError::Wait)?;
        std::fs::rename(temporary, &self.state).map_err(ProcessError::Wait)
    }
    fn allowance(&mut self, now: u64, timing: Timing) -> usize {
        let window = timing.window.as_millis() as u64;
        self.videos.retain(|time| time.saturating_add(window) > now);
        timing.max_videos.saturating_sub(self.videos.len())
    }
    fn quota_deadline(&self, timing: Timing) -> u64 {
        self.videos
            .iter()
            .min()
            .copied()
            .unwrap_or(0)
            .saturating_add(timing.window.as_millis() as u64)
    }
    fn record_video(&mut self, timing: Timing) -> Result<(), ProcessError> {
        let now = epoch_ms();
        if self.allowance(now, timing) == 0 {
            return Err(ProcessError::Wait(std::io::Error::other(
                "yt-dlp excedeu a reserva local de vídeos",
            )));
        }
        self.videos.push(now);
        self.save()
    }
    async fn wait(
        &self,
        cancel: &CancellationToken,
        notice: &mut impl FnMut(&str, Option<u64>),
    ) -> bool {
        let remaining = self.next_ms.saturating_sub(epoch_ms());
        if remaining == 0 {
            return !cancel.is_cancelled();
        }
        notice(
            "YouTube: aguardando intervalo de segurança",
            Some(remaining.div_ceil(1000)),
        );
        sleep(Duration::from_millis(remaining), cancel).await
    }
}
async fn sleep(duration: Duration, cancel: &CancellationToken) -> bool {
    tokio::select! { biased;
        _ = cancel.cancelled() => false,
        _ = tokio::time::sleep(duration) => true,
    }
}
fn cancelled() -> ProcessResult {
    ProcessResult {
        cancelled: true,
        ..Default::default()
    }
}

// yt-dlp normally does not expose response headers. Honor numeric Retry-After
// only if present in the diagnostic; otherwise use our explicit fallback.
fn retry_after(text: &str) -> Option<Duration> {
    text.lines()
        .filter_map(|line| {
            let value = line
                .trim()
                .strip_prefix("Retry-After:")
                .or_else(|| line.trim().strip_prefix("retry-after:"))?
                .trim();
            let seconds = value.parse::<u64>().ok()?;
            Some(Duration::from_secs(
                seconds.saturating_add(seconds.div_ceil(2)),
            ))
        })
        .max()
}
fn retry_delay(result: &ProcessResult, attempt: u32, timing: Timing) -> Option<Duration> {
    let text = String::from_utf8_lossy(&result.stderr);
    let lower = text.to_ascii_lowercase();
    // Login/permission refusals are not solved by repeating identical requests.
    if [
        "http error 401",
        "http error 403",
        "sign in",
        "login required",
        "private video",
        "video unavailable",
        "confirm you're not a bot",
    ]
    .iter()
    .any(|p| lower.contains(p))
    {
        return None;
    }
    let transient = result.timed_out
        || [
            "http error 429",
            "too many requests",
            "rate limit",
            "this content isn't available, try again later",
            "http error 500",
            "http error 502",
            "http error 503",
            "http error 504",
            "timed out",
            "connection reset",
            "temporary failure",
            "connection refused",
        ]
        .iter()
        .any(|p| lower.contains(p));
    if !transient {
        return None;
    }
    let fallback = timing.fallback.saturating_mul(1u32 << attempt.min(2));
    Some(fallback.max(retry_after(&text).unwrap_or_default()))
}

pub(crate) async fn run(
    request: Request<'_>,
    on_line: impl FnMut(&str) + Send,
    notice: impl FnMut(&str, Option<u64>) + Send,
) -> Result<ProcessResult, ProcessError> {
    run_with_timing(request, on_line, notice, TIMING).await
}
async fn run_with_timing(
    mut request: Request<'_>,
    mut on_line: impl FnMut(&str) + Send,
    mut notice: impl FnMut(&str, Option<u64>) + Send,
    timing: Timing,
) -> Result<ProcessResult, ProcessError> {
    let youtube = request.protection_enabled && is_youtube(request.url);
    if youtube {
        policy_args(&mut request.spec);
    }
    let mut gate = if youtube {
        match Gate::acquire(request.data_dir, &request.cancel, &mut notice).await? {
            Some(gate) => Some(gate),
            None => return Ok(cancelled()),
        }
    } else {
        None
    };
    let attempts = if youtube { timing.retries } else { 0 };
    let mut attempt = 0;
    loop {
        if request.cancel.is_cancelled() {
            return Ok(cancelled());
        }
        let mut spec = request.spec.clone();
        if let Some(gate) = &mut gate {
            if !request.capture {
                loop {
                    let allowance = gate.allowance(epoch_ms(), timing);
                    if allowance > 0 {
                        spec.args.extend([
                            "--max-downloads".into(),
                            allowance.to_string().into(),
                            "--print".into(),
                            "before_dl:POLICY_VIDEO %(id)j".into(),
                            "--no-simulate".into(),
                        ]);
                        break;
                    }
                    let remaining = gate.quota_deadline(timing).saturating_sub(epoch_ms());
                    notice(
                        &format!(
                            "Limite de {} vídeos em {} minutos atingido",
                            timing.max_videos,
                            timing.window.as_secs() / 60
                        ),
                        Some(remaining.div_ceil(1000)),
                    );
                    if !sleep(Duration::from_millis(remaining.max(1)), &request.cancel).await {
                        return Ok(cancelled());
                    }
                }
            }
            if !gate.wait(&request.cancel, &mut notice).await {
                return Ok(cancelled());
            }
            gate.postpone(timing.gap)?;
        }
        notice(
            &format!(
                "Conectando ao serviço · tentativa {} de {}",
                attempt + 1,
                attempts + 1
            ),
            Some(0),
        );
        let child_cancel = request.cancel.child_token();
        let mut quota_stop = false;
        let mut counted = 0usize;
        let mut recording_error = None;
        let result = if request.capture {
            TokioProcessRunner.run(spec, child_cancel).await
        } else {
            TokioProcessRunner
                .run_lines(spec, child_cancel.clone(), |line| {
                    if youtube && line.starts_with("POLICY_VIDEO ") {
                        if let Some(gate) = &mut gate {
                            match gate.record_video(timing) {
                                Ok(()) => counted += 1,
                                Err(error) => {
                                    recording_error = Some(error);
                                    child_cancel.cancel();
                                }
                            }
                        }
                    } else {
                        if line.contains("Maximum number of downloads reached") {
                            quota_stop = true;
                        }
                        on_line(line);
                    }
                })
                .await
        };
        if let Some(gate) = &mut gate {
            gate.postpone(timing.gap)?;
        }
        if let Some(error) = recording_error {
            return Err(error);
        }
        let result = result?;
        if result.cancelled || result.code == Some(0) || !youtube {
            return Ok(result);
        }
        if result.code == Some(101) && quota_stop && counted > 0 {
            // The archive retains completed entries. Resume the same playlist
            // after the sliding window releases slots, without retry-budget cost.
            if request.spec.args.iter().any(|a| a == "--no-playlist") {
                return Ok(ProcessResult {
                    code: Some(0),
                    ..result
                });
            }
            attempt = 0;
            continue;
        }
        let Some(delay) = retry_delay(&result, attempt, timing) else {
            return Ok(result);
        };
        if let Some(gate) = &mut gate {
            gate.postpone(delay)?;
        }
        if attempt == attempts {
            notice(
                "Limite de tentativas atingido; intervalo de segurança mantido",
                Some(delay.as_secs()),
            );
            return Ok(result);
        }
        notice(
            &format!(
                "YouTube recusou ou interrompeu a operação; nova tentativa em {} s",
                delay.as_secs()
            ),
            Some(delay.as_secs()),
        );
        attempt += 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn timing() -> Timing {
        Timing {
            gap: Duration::from_millis(1),
            fallback: Duration::from_millis(5),
            retries: 3,
            max_videos: 2,
            window: Duration::from_millis(80),
        }
    }
    fn script(dir: &Path, body: &str) -> ProcessSpec {
        let path = dir.join("fake-yt-dlp");
        std::fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)).unwrap();
        ProcessSpec {
            executable: path.into(),
            current_dir: Some(dir.into()),
            timeout: Some(Duration::from_secs(2)),
            ..Default::default()
        }
    }
    fn request<'a>(spec: ProcessSpec, dir: &'a Path, cancel: CancellationToken) -> Request<'a> {
        Request {
            spec,
            url: "https://www.youtube.com/playlist?list=test",
            data_dir: dir,
            cancel,
            capture: false,
            protection_enabled: true,
        }
    }
    #[test]
    fn youtube_scope_and_requested_delays() {
        assert!(is_youtube("https://music.youtube.com/watch?v=a"));
        assert!(is_youtube("https://youtu.be/a"));
        assert!(!is_youtube("https://youtube.com.evil.test/a"));
        assert!(!is_youtube("https://youtube.com@evil.test/a"));
        let mut spec = ProcessSpec::default();
        policy_args(&mut spec);
        assert!(spec.args.windows(2).any(|w| w == ["--sleep-interval", "5"]));
        assert!(spec
            .args
            .windows(2)
            .any(|w| w == ["--max-sleep-interval", "5"]));
        assert_eq!(TIMING.max_videos, 300);
        assert_eq!(TIMING.window.as_secs(), 5400);
    }
    #[test]
    fn retry_classification_and_fifty_percent_margin() {
        let failure = |text: &str| ProcessResult {
            code: Some(1),
            stderr: text.as_bytes().to_vec(),
            ..Default::default()
        };
        assert_eq!(
            retry_delay(&failure("HTTP Error 429"), 0, TIMING),
            Some(Duration::from_secs(90))
        );
        assert_eq!(
            retry_delay(&failure("HTTP Error 503"), 1, TIMING),
            Some(Duration::from_secs(180))
        );
        assert_eq!(
            retry_delay(&failure("HTTP Error 429\nRetry-After: 400"), 0, TIMING),
            Some(Duration::from_secs(600))
        );
        assert_eq!(retry_after("Retry-After: 3"), Some(Duration::from_secs(5)));
        assert!(retry_delay(&failure("HTTP Error 403: Forbidden"), 0, TIMING).is_none());
        assert!(retry_delay(&failure("Sign in to confirm you're not a bot"), 0, TIMING).is_none());
        assert!(retry_delay(&failure("Unsupported URL"), 0, TIMING).is_none());
    }
    #[tokio::test]
    async fn quota_is_sliding_persistent_and_shared() {
        let dir = tempfile::tempdir().unwrap();
        let token = CancellationToken::new();
        let mut gate = Gate::acquire(dir.path(), &token, &mut |_, _| {})
            .await
            .unwrap()
            .unwrap();
        let policy = timing();
        gate.videos = vec![100, 120];
        assert_eq!(gate.allowance(179, policy), 0);
        assert_eq!(gate.allowance(180, policy), 1);
        assert_eq!(gate.videos, vec![120]);
        gate.save().unwrap();
        drop(gate);
        let gate = Gate::acquire(dir.path(), &token, &mut |_, _| {})
            .await
            .unwrap()
            .unwrap();
        assert_eq!(gate.videos, vec![120]);
        let blocked = CancellationToken::new();
        blocked.cancel();
        assert!(Gate::acquire(dir.path(), &blocked, &mut |_, _| {})
            .await
            .unwrap()
            .is_none());
    }
    #[tokio::test]
    async fn retry_exhaustion_is_bounded_and_cooldown_survives() {
        let dir = tempfile::tempdir().unwrap();
        let spec = script(
            dir.path(),
            "echo run >> attempts\necho 'HTTP Error 429: Too Many Requests' >&2\nexit 1",
        );
        let result = run_with_timing(
            request(spec, dir.path(), CancellationToken::new()),
            |_| {},
            |_, _| {},
            timing(),
        )
        .await
        .unwrap();
        assert_eq!(result.code, Some(1));
        assert_eq!(
            std::fs::read_to_string(dir.path().join("attempts"))
                .unwrap()
                .lines()
                .count(),
            4
        );
        let state: serde_json::Value = serde_json::from_slice(
            &std::fs::read(dir.path().join("youtube-network.json")).unwrap(),
        )
        .unwrap();
        assert!(state["next_ms"].as_u64().unwrap() > epoch_ms());
    }
    #[tokio::test]
    async fn authentication_failure_does_not_retry() {
        let dir = tempfile::tempdir().unwrap();
        let spec = script(
            dir.path(),
            "echo run >> attempts\necho 'HTTP Error 403: Forbidden' >&2\nexit 1",
        );
        let result = run_with_timing(
            request(spec, dir.path(), CancellationToken::new()),
            |_| {},
            |_, _| {},
            timing(),
        )
        .await
        .unwrap();
        assert_eq!(result.code, Some(1));
        assert_eq!(
            std::fs::read_to_string(dir.path().join("attempts"))
                .unwrap()
                .lines()
                .count(),
            1
        );
    }
    #[tokio::test]
    async fn cancellation_interrupts_retry_wait() {
        let dir = tempfile::tempdir().unwrap();
        let spec = script(
            dir.path(),
            "echo run >> attempts\necho 'HTTP Error 429' >&2\nexit 1",
        );
        let cancel = CancellationToken::new();
        let result = run_with_timing(
            request(spec, dir.path(), cancel.clone()),
            |_| {},
            |message, _| {
                if message.contains("nova tentativa") {
                    cancel.cancel();
                }
            },
            TIMING,
        )
        .await
        .unwrap();
        assert!(result.cancelled);
        assert_eq!(
            std::fs::read_to_string(dir.path().join("attempts"))
                .unwrap()
                .lines()
                .count(),
            1
        );
    }
    #[tokio::test]
    async fn quota_wait_resumes_without_consuming_retry_budget() {
        let dir = tempfile::tempdir().unwrap();
        let spec = script(dir.path(), "if [ ! -f first ]; then\ntouch first\necho 'POLICY_VIDEO one'\necho 'FILE one'\necho 'POLICY_VIDEO two'\necho 'FILE two'\necho 'Maximum number of downloads reached'\nexit 101\nfi\necho 'POLICY_VIDEO three'\necho 'FILE three'");
        let mut records = Vec::new();
        let mut notices = Vec::new();
        let start = std::time::Instant::now();
        let result = run_with_timing(
            request(spec, dir.path(), CancellationToken::new()),
            |line| records.push(line.to_owned()),
            |message, _| notices.push(message.to_owned()),
            timing(),
        )
        .await
        .unwrap();
        assert_eq!(result.code, Some(0));
        assert!(start.elapsed() >= timing().window);
        assert_eq!(
            records
                .iter()
                .filter(|line| line.starts_with("FILE "))
                .count(),
            3
        );
        assert!(notices.iter().any(|s| s.contains("Limite de 2 vídeos")));
        let state: serde_json::Value = serde_json::from_slice(
            &std::fs::read(dir.path().join("youtube-network.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(state["videos"].as_array().unwrap().len(), 1);
    }
    #[tokio::test]
    async fn cancellation_interrupts_full_window_without_starting_process() {
        let dir = tempfile::tempdir().unwrap();
        let saved = serde_json::json!({"next_ms": 0, "videos": vec![epoch_ms(); 300]});
        std::fs::write(dir.path().join("youtube-network.json"), saved.to_string()).unwrap();
        let spec = script(dir.path(), "touch should-not-run");
        let cancel = CancellationToken::new();
        let result = run_with_timing(
            request(spec, dir.path(), cancel.clone()),
            |_| {},
            |message, _| {
                if message.contains("Limite de 300") {
                    cancel.cancel();
                }
            },
            TIMING,
        )
        .await
        .unwrap();
        assert!(result.cancelled);
        assert!(!dir.path().join("should-not-run").exists());
    }
}
