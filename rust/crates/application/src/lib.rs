//! Coordenação da fila e das transições de tarefas.
//!
//! O crate não conhece a UI, SQLite ou subprocessos. Serviços concretos são
//! conectados pela camada de integração e publicam `TaskEvent` para a UI.

use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use downloader_domain::{
    AccelerationMode, AppCommand, AppError, ErrorCategory, TaskEvent, TaskId, TaskRecord,
    TaskStatus,
};
use serde_json::Value;

pub mod engine;
mod library;
mod network;
pub use engine::{EngineConfig, MediaEngine};

/// Adapta o repositório SQLite da infraestrutura ao contrato consumido pela
/// aplicação. A conversão mantém `options` como objeto JSON e transforma
/// qualquer falha em erro categorizado, sem vazar detalhes de credenciais.
impl TaskPersistence for downloader_infra::SqliteTaskRepository {
    fn load_tasks(&self) -> Result<Vec<TaskRecord>, AppError> {
        self.list_records().map_err(storage_error)
    }

    fn save_task(&self, task: &TaskRecord) -> Result<(), AppError> {
        self.put_record(task).map_err(storage_error)
    }
}

fn storage_error(error: downloader_infra::SqliteError) -> AppError {
    AppError {
        category: ErrorCategory::Internal,
        message: format!("Falha ao persistir tarefa: {error}"),
        step: Some("persistência".into()),
        code: None,
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QueueLimits {
    pub max_downloads: usize,
    pub max_conversions: usize,
}

/// Persistência mínima exigida pela camada de aplicação.
///
/// A implementação concreta fica em `infra` (SQLite). O contrato usa os
/// mesmos registros do domínio para impedir uma segunda representação de
/// opções, estados ou erros.
pub trait TaskPersistence: Send + Sync {
    fn load_tasks(&self) -> Result<Vec<TaskRecord>, AppError>;
    fn save_task(&self, task: &TaskRecord) -> Result<(), AppError>;
}

/// Lê a preferência de aceleração armazenada nas opções de uma tarefa.
/// Ausência significa `Auto`; valores desconhecidos são rejeitados antes de
/// a tarefa entrar na fila, evitando que uma string arbitrária vire argumento
/// de processo.
pub fn parse_acceleration_mode(options: &Value) -> Result<AccelerationMode, AppError> {
    let raw = options
        .get("aceleracao")
        .or_else(|| options.get("acceleration"))
        .and_then(Value::as_str)
        .unwrap_or("auto")
        .to_ascii_lowercase();
    let mode = match raw.as_str() {
        "auto" => AccelerationMode::Auto,
        "software" | "cpu" => AccelerationMode::Software,
        "vulkan" => AccelerationMode::Vulkan,
        "vaapi" => AccelerationMode::Vaapi,
        "amf" => AccelerationMode::Amf,
        "cuda" | "nvenc" => AccelerationMode::Cuda,
        "qsv" => AccelerationMode::Qsv,
        _ => {
            return Err(AppError {
                category: ErrorCategory::Internal,
                message: format!("Backend de aceleração desconhecido: {raw}"),
                step: Some("planejamento".into()),
                code: None,
            });
        }
    };
    Ok(mode)
}

/// Detecta um navegador local que o yt-dlp consegue usar como fonte de
/// cookies. A função só verifica diretórios de perfil: nunca abre, lê ou
/// copia bancos de cookies. A ordem é determinística para que a tarefa seja
/// reproduzível quando mais de um navegador estiver instalado.
pub fn detect_browser_profile() -> Option<String> {
    let home = std::env::var_os("HOME").map(PathBuf::from)?;
    let config = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home.join(".config"));
    detect_browser_profile_in(&home, &config)
}

fn detect_browser_profile_in(home: &Path, config: &Path) -> Option<String> {
    let candidates = [
        ("firefox", home.join(".mozilla").join("firefox")),
        ("chrome", config.join("google-chrome")),
        ("chromium", config.join("chromium")),
        ("brave", config.join("BraveSoftware").join("Brave-Browser")),
        ("edge", config.join("microsoft-edge")),
        ("vivaldi", config.join("vivaldi")),
        ("opera", config.join("opera")),
    ];
    candidates
        .into_iter()
        .find_map(|(browser, path)| path.is_dir().then_some(browser.to_owned()))
}

impl Default for QueueLimits {
    fn default() -> Self {
        Self {
            max_downloads: 2,
            max_conversions: 1,
        }
    }
}

#[derive(Debug)]
pub struct ApplicationState {
    tasks: HashMap<TaskId, TaskRecord>,
    pending: VecDeque<TaskId>,
    limits: QueueLimits,
    active_downloads: usize,
    active_conversions: usize,
    next_sequence: u64,
}

impl ApplicationState {
    pub fn new(limits: QueueLimits) -> Self {
        Self {
            tasks: HashMap::new(),
            pending: VecDeque::new(),
            limits,
            active_downloads: 0,
            active_conversions: 0,
            next_sequence: 0,
        }
    }

    pub fn tasks(&self) -> impl Iterator<Item = &TaskRecord> {
        self.tasks.values()
    }

    pub fn task(&self, id: &str) -> Option<&TaskRecord> {
        self.tasks.get(id)
    }

    /// Restaura um registro vindo do armazenamento, sem criar um evento de UI.
    /// Tarefas na fila são recolocadas na fila; tarefas ativas serão marcadas
    /// como interrompidas pelo serviço de abertura.
    pub fn restore(&mut self, task: TaskRecord) -> Result<(), AppError> {
        if self.tasks.contains_key(&task.id) {
            return Err(AppError {
                category: ErrorCategory::Internal,
                message: "ID de tarefa duplicado no armazenamento.".into(),
                step: Some("persistência".into()),
                code: None,
            });
        }
        let id = task.id.clone();
        if task.status == TaskStatus::NaFila {
            self.pending.push_back(id.clone());
        }
        self.tasks.insert(id, task);
        Ok(())
    }

    pub fn enqueue(&mut self, mut task: TaskRecord) -> Result<TaskEvent, AppError> {
        if self.tasks.contains_key(&task.id) {
            return Err(AppError {
                category: ErrorCategory::Internal,
                message: "ID de tarefa já existe.".into(),
                step: Some("fila".into()),
                code: None,
            });
        }
        parse_acceleration_mode(&task.options)?;
        task.status = TaskStatus::NaFila;
        task.updated_at_ms = now_ms();
        let id = task.id.clone();
        self.tasks.insert(id.clone(), task);
        self.pending.push_back(id.clone());
        Ok(self.event(&id, None, Some("Tarefa adicionada à fila.".into())))
    }

    pub fn recover_after_restart(&mut self) -> Vec<TaskEvent> {
        let ids: Vec<_> = self
            .tasks
            .values_mut()
            .filter(|t| t.is_active())
            .map(|t| {
                t.status = TaskStatus::Interrompido;
                t.updated_at_ms = now_ms();
                t.id.clone()
            })
            .collect();
        ids.iter()
            .map(|id| {
                self.event(
                    id,
                    None,
                    Some("Tarefa interrompida ao reabrir o aplicativo.".into()),
                )
            })
            .collect()
    }

    pub fn cancel(&mut self, id: &str) -> Result<TaskEvent, AppError> {
        let task = self.tasks.get_mut(id).ok_or_else(|| Self::not_found(id))?;
        if task.status.terminal() {
            return Err(AppError {
                category: ErrorCategory::Internal,
                message: "A tarefa já terminou.".into(),
                step: Some("fila".into()),
                code: None,
            });
        }
        task.status = TaskStatus::Cancelado;
        task.updated_at_ms = now_ms();
        self.pending.retain(|pending_id| pending_id != id);
        Ok(self.event(id, None, Some("Cancelamento solicitado.".into())))
    }

    pub fn retry(&mut self, id: &str) -> Result<TaskEvent, AppError> {
        let task = self.tasks.get_mut(id).ok_or_else(|| Self::not_found(id))?;
        if !matches!(
            task.status,
            TaskStatus::Falhou | TaskStatus::Cancelado | TaskStatus::Interrompido
        ) {
            return Err(AppError {
                category: ErrorCategory::Internal,
                message: "Só tarefas falhas, canceladas ou interrompidas podem ser repetidas."
                    .into(),
                step: Some("fila".into()),
                code: None,
            });
        }
        task.status = TaskStatus::NaFila;
        task.error = None;
        task.updated_at_ms = now_ms();
        self.pending.push_back(id.to_owned());
        Ok(self.event(id, None, Some("Tarefa retornou à fila.".into())))
    }

    pub fn start_next(&mut self) -> Option<TaskEvent> {
        let position = self.pending.iter().position(|id| self.can_start(id))?;
        let id = self.pending.remove(position)?;
        let task = self.tasks.get_mut(&id)?;
        task.status = match task.kind {
            downloader_domain::TaskKind::Download => TaskStatus::Baixando,
            downloader_domain::TaskKind::Convert => TaskStatus::Convertendo,
            downloader_domain::TaskKind::Validate => TaskStatus::Validando,
            downloader_domain::TaskKind::Merge => TaskStatus::Unindo,
        };
        task.updated_at_ms = now_ms();
        if matches!(task.kind, downloader_domain::TaskKind::Download) {
            self.active_downloads += 1;
        }
        if matches!(
            task.kind,
            downloader_domain::TaskKind::Convert | downloader_domain::TaskKind::Merge
        ) {
            self.active_conversions += 1;
        }
        Some(self.event(&id, None, Some("Tarefa iniciada.".into())))
    }

    pub fn finish(
        &mut self,
        id: &str,
        status: TaskStatus,
        error: Option<AppError>,
    ) -> Result<TaskEvent, AppError> {
        let task = self.tasks.get_mut(id).ok_or_else(|| Self::not_found(id))?;
        if !matches!(
            status,
            TaskStatus::Concluido | TaskStatus::Falhou | TaskStatus::Cancelado
        ) {
            return Err(AppError {
                category: ErrorCategory::Internal,
                message: "Estado final inválido.".into(),
                step: Some("fila".into()),
                code: None,
            });
        }
        if matches!(task.kind, downloader_domain::TaskKind::Download) && self.active_downloads > 0 {
            self.active_downloads -= 1;
        }
        if matches!(
            task.kind,
            downloader_domain::TaskKind::Convert | downloader_domain::TaskKind::Merge
        ) && self.active_conversions > 0
        {
            self.active_conversions -= 1;
        }
        task.status = status;
        task.error = error;
        task.updated_at_ms = now_ms();
        Ok(self.event(id, None, None))
    }

    pub fn dispatch(&mut self, command: AppCommand) -> Result<Vec<TaskEvent>, AppError> {
        match command {
            AppCommand::Enqueue(task) => Ok(vec![self.enqueue(*task)?]),
            AppCommand::Cancel { task_id } => Ok(vec![self.cancel(&task_id)?]),
            AppCommand::Retry { task_id } => Ok(vec![self.retry(&task_id)?]),
            AppCommand::ListTasks
            | AppCommand::Analyze { .. }
            | AppCommand::UpdateSettings { .. } => Ok(Vec::new()),
        }
    }

    fn can_start(&self, id: &str) -> bool {
        let Some(task) = self.tasks.get(id) else {
            return false;
        };
        match task.kind {
            downloader_domain::TaskKind::Download => {
                self.active_downloads < self.limits.max_downloads
            }
            downloader_domain::TaskKind::Convert | downloader_domain::TaskKind::Merge => {
                self.active_conversions < self.limits.max_conversions
            }
            downloader_domain::TaskKind::Validate => true,
        }
    }

    fn event(
        &mut self,
        id: &str,
        stage: Option<downloader_domain::PlanStage>,
        message: Option<String>,
    ) -> TaskEvent {
        self.next_sequence += 1;
        let task = self.tasks.get(id);
        let status = task.map_or(TaskStatus::Falhou, |task| task.status.clone());
        let message = message.or_else(|| {
            task.and_then(|task| {
                task.error.as_ref().map(|error| {
                    format!(
                        "{}: {}",
                        error.step.as_deref().unwrap_or("tarefa"),
                        error.message
                    )
                })
            })
        });
        TaskEvent {
            task_id: id.to_owned(),
            sequence: self.next_sequence,
            status,
            stage,
            progress: None,
            speed: None,
            eta_seconds: None,
            message,
            destination: task.and_then(|task| task.destination.clone()),
        }
    }

    fn not_found(id: &str) -> AppError {
        AppError {
            category: ErrorCategory::Internal,
            message: format!("Tarefa não encontrada: {id}"),
            step: Some("fila".into()),
            code: None,
        }
    }
}

/// Fachada que conecta a fila a uma persistência concreta.
///
/// Ela centraliza a regra de persistir toda transição observável. O executor
/// de mídia ainda é injetado em uma etapa posterior; por enquanto a fachada
/// expõe as transições seguras que o executor deverá consumir.
pub struct ApplicationService<P> {
    state: ApplicationState,
    persistence: P,
}

impl<P: TaskPersistence> ApplicationService<P> {
    pub fn open(persistence: P, limits: QueueLimits) -> Result<(Self, Vec<TaskEvent>), AppError> {
        let mut state = ApplicationState::new(limits);
        for task in persistence.load_tasks()? {
            state.restore(task)?;
        }
        let recovery = state.recover_after_restart();
        for task in state.tasks() {
            persistence.save_task(task)?;
        }
        Ok((Self { state, persistence }, recovery))
    }

    pub fn state(&self) -> &ApplicationState {
        &self.state
    }

    pub fn dispatch(&mut self, command: AppCommand) -> Result<Vec<TaskEvent>, AppError> {
        let events = self.state.dispatch(command)?;
        self.persist_event_tasks(&events)?;
        Ok(events)
    }

    pub fn start_next(&mut self) -> Result<Option<TaskEvent>, AppError> {
        let event = self.state.start_next();
        if let Some(ref event) = event {
            self.persist_event_tasks(std::slice::from_ref(event))?;
        }
        Ok(event)
    }

    pub fn finish(
        &mut self,
        id: &str,
        status: TaskStatus,
        error: Option<AppError>,
    ) -> Result<TaskEvent, AppError> {
        let event = self.state.finish(id, status, error)?;
        self.persist_event_tasks(std::slice::from_ref(&event))?;
        Ok(event)
    }

    /// Atualiza o caminho publicado sem expor a representação interna da fila.
    pub fn set_destination(&mut self, id: &str, destination: String) -> Result<(), AppError> {
        let task = self
            .state
            .tasks
            .get_mut(id)
            .ok_or_else(|| ApplicationState::not_found(id))?;
        task.destination = Some(destination);
        task.updated_at_ms = now_ms();
        self.persistence.save_task(task).map_err(|error| AppError {
            category: ErrorCategory::Internal,
            message: format!("Falha ao persistir destino: {error:?}"),
            step: Some("persistência".into()),
            code: None,
        })
    }

    fn persist_event_tasks(&self, events: &[TaskEvent]) -> Result<(), AppError> {
        for event in events {
            let task = self.state.task(&event.task_id).ok_or_else(|| AppError {
                category: ErrorCategory::Internal,
                message: "Evento referencia tarefa ausente.".into(),
                step: Some("persistência".into()),
                code: None,
            })?;
            self.persistence.save_task(task)?;
        }
        Ok(())
    }
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_millis() as u64)
}

#[cfg(test)]
mod tests {
    use super::*;
    use downloader_domain::{CollisionPolicy, TaskKind};

    fn task(id: &str, kind: TaskKind) -> TaskRecord {
        TaskRecord {
            id: id.into(),
            kind,
            input_url: None,
            input_path: None,
            options: serde_json::json!({}),
            destination: None,
            collision: CollisionPolicy::Rename,
            auth: None,
            status: TaskStatus::NaFila,
            temporary_path: None,
            error: None,
            created_at_ms: 0,
            updated_at_ms: 0,
        }
    }

    #[test]
    fn conversion_limit_is_enforced() {
        let mut app = ApplicationState::new(QueueLimits {
            max_downloads: 2,
            max_conversions: 1,
        });
        app.enqueue(task("a", TaskKind::Convert))
            .expect("enqueue a");
        app.enqueue(task("b", TaskKind::Convert))
            .expect("enqueue b");
        assert_eq!(app.start_next().expect("first").task_id, "a");
        assert!(app.start_next().is_none());
        app.finish("a", TaskStatus::Concluido, None)
            .expect("finish");
        assert_eq!(app.start_next().expect("second").task_id, "b");
    }

    #[test]
    fn active_tasks_become_interrupted_on_recovery() {
        let mut app = ApplicationState::new(QueueLimits::default());
        let mut t = task("a", TaskKind::Download);
        t.status = TaskStatus::Baixando;
        app.tasks.insert(t.id.clone(), t);
        let events = app.recover_after_restart();
        assert_eq!(events.len(), 1);
        assert_eq!(
            app.task("a").expect("task").status,
            TaskStatus::Interrompido
        );
    }

    #[derive(Default)]
    struct MemoryPersistence {
        tasks: std::sync::Mutex<Vec<TaskRecord>>,
    }

    impl TaskPersistence for MemoryPersistence {
        fn load_tasks(&self) -> Result<Vec<TaskRecord>, AppError> {
            Ok(self.tasks.lock().expect("lock").clone())
        }

        fn save_task(&self, task: &TaskRecord) -> Result<(), AppError> {
            let mut tasks = self.tasks.lock().expect("lock");
            if let Some(existing) = tasks.iter_mut().find(|existing| existing.id == task.id) {
                *existing = task.clone();
            } else {
                tasks.push(task.clone());
            }
            Ok(())
        }
    }

    #[test]
    fn service_persists_enqueue_and_recovery() {
        let persistence = MemoryPersistence::default();
        let (mut service, recovery) =
            ApplicationService::open(persistence, QueueLimits::default()).expect("open");
        assert!(recovery.is_empty());
        service
            .dispatch(AppCommand::Enqueue(Box::new(task("a", TaskKind::Download))))
            .expect("enqueue");
        assert_eq!(
            service.state().task("a").expect("task").status,
            TaskStatus::NaFila
        );
    }

    #[test]
    fn acceleration_option_is_validated_before_enqueue() {
        let mut app = ApplicationState::new(QueueLimits::default());
        let mut invalid = task("invalid", TaskKind::Convert);
        invalid.options = serde_json::json!({"aceleracao": "backend-inexistente"});
        let error = app.enqueue(invalid).expect_err("backend inválido");
        assert!(error.message.contains("desconhecido"));
        assert_eq!(
            parse_acceleration_mode(&serde_json::json!({})).unwrap(),
            AccelerationMode::Auto
        );
    }

    #[test]
    fn automatic_browser_detection_is_deterministic_and_does_not_read_cookies() {
        let root = tempfile::tempdir().expect("tempdir");
        let home = root.path().join("home");
        let config = home.join(".config");
        std::fs::create_dir_all(config.join("chromium")).expect("profile");
        assert_eq!(
            detect_browser_profile_in(&home, &config).as_deref(),
            Some("chromium")
        );
    }
}
