//! Contratos compartilhados entre a aplicação Downloader em Rust.
//!
//! Este crate não executa rede, processos ou acesso ao banco. Ele contém os
//! tipos que permitem que a UI, a fila e os serviços avancem sem duplicar
//! decisões de estado ou o formato persistido.

use serde::{Deserialize, Serialize};
use serde_json::Value;

pub type TaskId = String;

/// Preferência de aceleração para uma tarefa de mídia. `Auto` só deve ser
/// resolvido para um backend que passou na sondagem de runtime; caso contrário
/// a execução cai para `Software`.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum AccelerationMode {
    #[default]
    Auto,
    Software,
    Vulkan,
    Vaapi,
    Amf,
    Cuda,
    Qsv,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TaskKind {
    Download,
    Merge,
    Convert,
    Validate,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TaskStatus {
    NaFila,
    Analisando,
    AguardandoAutenticacao,
    Baixando,
    Unindo,
    Convertendo,
    Validando,
    Concluido,
    Falhou,
    Cancelado,
    Interrompido,
}

impl TaskStatus {
    pub fn terminal(&self) -> bool {
        matches!(self, Self::Concluido | Self::Falhou | Self::Cancelado)
    }

    pub fn active(&self) -> bool {
        matches!(
            self,
            Self::Analisando
                | Self::AguardandoAutenticacao
                | Self::Baixando
                | Self::Unindo
                | Self::Convertendo
                | Self::Validando
        )
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum CollisionPolicy {
    #[default]
    Rename,
    Replace,
    Skip,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AuthRef {
    /// For `CookieFile`, this is the selected Netscape file path. For
    /// `BrowserProfile`, it is the yt-dlp selector such as `firefox:default`.
    /// Cookie values themselves are never stored here.
    pub id: String,
    pub kind: AuthKind,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum AuthKind {
    CookieFile,
    BrowserProfile,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct TaskRecord {
    pub id: TaskId,
    pub kind: TaskKind,
    pub input_url: Option<String>,
    pub input_path: Option<String>,
    /// Opções tipadas pela camada de domínio e armazenadas como JSON uma vez.
    pub options: Value,
    pub destination: Option<String>,
    pub collision: CollisionPolicy,
    pub auth: Option<AuthRef>,
    pub status: TaskStatus,
    pub temporary_path: Option<String>,
    pub error: Option<AppError>,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
}

impl TaskRecord {
    pub fn is_active(&self) -> bool {
        self.status.active()
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct StreamInfo {
    pub index: u32,
    pub kind: StreamKind,
    pub codec: Option<String>,
    pub language: Option<String>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub fps: Option<f64>,
    pub bitrate: Option<u64>,
    pub channels: Option<u16>,
    pub sample_rate: Option<u32>,
    pub duration_ms: Option<u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum StreamKind {
    Video,
    Audio,
    Subtitle,
    Other,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct FormatInfo {
    pub id: String,
    pub container: Option<String>,
    pub video_codec: Option<String>,
    pub audio_codec: Option<String>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub fps: Option<f64>,
    pub bitrate: Option<u64>,
    pub language: Option<String>,
    pub has_video: bool,
    pub has_audio: bool,
    pub size_bytes: Option<u64>,
    pub size_is_estimate: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct MediaInfo {
    pub title: Option<String>,
    pub uploader: Option<String>,
    pub duration_ms: Option<u64>,
    pub extractor: Option<String>,
    pub formats: Vec<FormatInfo>,
    pub streams: Vec<StreamInfo>,
    pub playlist: Option<PlaylistInfo>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct PlaylistInfo {
    pub id: Option<String>,
    pub title: Option<String>,
    pub entries: Vec<PlaylistEntry>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct PlaylistEntry {
    pub id: String,
    pub title: Option<String>,
    pub url: Option<String>,
    pub duration_ms: Option<u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct OutputPlan {
    pub source_container: Option<String>,
    pub output_container: String,
    pub selected_streams: Vec<u32>,
    pub video_codec: Option<String>,
    pub audio_codec: Option<String>,
    pub copy_video: bool,
    pub copy_audio: bool,
    pub expected_duration_ms: Option<u64>,
    pub expected_width: Option<u32>,
    pub expected_height: Option<u32>,
    pub stages: Vec<PlanStage>,
}

impl OutputPlan {
    pub fn needs_reencode(&self) -> bool {
        !self.copy_video || !self.copy_audio
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum PlanStage {
    Download,
    Merge,
    Convert,
    Validate,
    Publish,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProcessSpec {
    pub executable: String,
    pub args: Vec<String>,
    pub working_directory: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProcessResult {
    pub return_code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
    pub termination: ProcessTermination,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ProcessTermination {
    Exited,
    TimedOut,
    Cancelled,
    SpawnFailed,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct TaskEvent {
    pub task_id: TaskId,
    pub sequence: u64,
    pub status: TaskStatus,
    pub stage: Option<PlanStage>,
    pub progress: Option<f32>,
    pub speed: Option<String>,
    pub eta_seconds: Option<u64>,
    pub message: Option<String>,
    pub destination: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub enum AppCommand {
    Analyze {
        url: String,
        auth: Option<AuthRef>,
        /// Permite consultar uma playlist sem extrair cada vídeo por completo.
        playlist: bool,
    },
    /// Box mantém o enum pequeno mesmo quando uma tarefa contém opções e erro.
    Enqueue(Box<TaskRecord>),
    Cancel {
        task_id: TaskId,
    },
    Retry {
        task_id: TaskId,
    },
    UpdateSettings {
        values: Value,
    },
    ListTasks,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AppError {
    pub category: ErrorCategory,
    pub message: String,
    pub step: Option<String>,
    pub code: Option<i32>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ErrorCategory {
    InvalidUrl,
    Authentication,
    Dependency,
    Network,
    FormatUnavailable,
    EncoderUnavailable,
    Permission,
    DiskSpace,
    InvalidMedia,
    Cancelled,
    Timeout,
    Internal,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_active_and_terminal_are_explicit() {
        assert!(TaskStatus::Baixando.active());
        assert!(!TaskStatus::Interrompido.active());
        assert!(TaskStatus::Concluido.terminal());
        assert!(!TaskStatus::NaFila.terminal());
    }

    #[test]
    fn task_record_round_trips_as_one_json_object() {
        let record = TaskRecord {
            id: "t1".into(),
            kind: TaskKind::Convert,
            input_url: None,
            input_path: Some("/tmp/in vídeo.mkv".into()),
            options: serde_json::json!({"format": "mp4"}),
            destination: None,
            collision: CollisionPolicy::Rename,
            auth: None,
            status: TaskStatus::NaFila,
            temporary_path: None,
            error: None,
            created_at_ms: 1,
            updated_at_ms: 1,
        };
        let encoded = serde_json::to_string(&record).expect("serialize");
        let decoded: TaskRecord = serde_json::from_str(&encoded).expect("deserialize");
        assert_eq!(decoded, record);
        assert!(decoded.options.is_object());
    }
}
