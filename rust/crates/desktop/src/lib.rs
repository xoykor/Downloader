//! Interface desktop egui/eframe. A UI emite comandos de domínio e consome
//! snapshots/eventos publicados pela camada de aplicação.

use downloader_application::ApplicationState;
use downloader_domain::{
    AccelerationMode, AppCommand, AppError, AuthKind, AuthRef, CollisionPolicy, TaskEvent,
    TaskKind, TaskRecord, TaskStatus,
};
use eframe::egui;
use std::sync::{Arc, Mutex};
use tokio::sync::mpsc;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Screen {
    Downloads,
    Conversor,
    FilaHistorico,
    Configuracoes,
}

impl Screen {
    pub const ALL: [Self; 4] = [
        Self::Downloads,
        Self::Conversor,
        Self::FilaHistorico,
        Self::Configuracoes,
    ];
    pub const fn label(self) -> &'static str {
        match self {
            Self::Downloads => "Downloads",
            Self::Conversor => "Conversor",
            Self::FilaHistorico => "Fila / Histórico",
            Self::Configuracoes => "Configurações",
        }
    }
}

/// Evento local de navegação ou comando que deve ser encaminhado à aplicação.
#[derive(Debug, Clone, PartialEq)]
pub enum UiEvent {
    Navegar(Screen),
    Comando(AppCommand),
    PausarFila,
    RetomarFila,
}

/// Ponte assíncrona entre a UI e o estado da aplicação.
#[derive(Clone)]
pub struct DesktopRuntime {
    commands: mpsc::UnboundedSender<AppCommand>,
    events: Arc<Mutex<mpsc::UnboundedReceiver<TaskEvent>>>,
}

impl DesktopRuntime {
    pub fn spawn() -> Self {
        Self::spawn_with_config(
            downloader_application::EngineConfig::default(),
            downloader_application::QueueLimits::default(),
        )
    }

    pub fn spawn_with(limits: downloader_application::QueueLimits) -> Self {
        Self::spawn_with_config(downloader_application::EngineConfig::default(), limits)
    }

    pub fn spawn_with_config(
        config: downloader_application::EngineConfig,
        limits: downloader_application::QueueLimits,
    ) -> Self {
        let (commands, command_rx) = mpsc::unbounded_channel();
        let (event_tx, event_rx) = mpsc::unbounded_channel();
        std::thread::Builder::new()
            .name("downloader-application".into())
            .spawn(move || {
                let runtime = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .expect("runtime Tokio da aplicação");
                runtime.block_on(async move {
                    match downloader_application::MediaEngine::open_with_limits(config, limits) {
                        Ok((engine, recovery)) => {
                            for event in recovery {
                                let _ = event_tx.send(event);
                            }
                            engine.run(command_rx, event_tx).await;
                        }
                        Err(error) => {
                            let _ = event_tx.send(TaskEvent {
                                task_id: "sistema".into(),
                                sequence: 0,
                                status: TaskStatus::Falhou,
                                stage: None,
                                progress: None,
                                speed: None,
                                eta_seconds: None,
                                message: Some(error.message),
                                destination: None,
                            });
                        }
                    }
                });
            })
            .expect("thread da aplicação");
        Self {
            commands,
            events: Arc::new(Mutex::new(event_rx)),
        }
    }

    pub fn send(&self, command: AppCommand) -> Result<(), mpsc::error::SendError<AppCommand>> {
        self.commands.send(command)
    }
    pub fn poll_events(&self) -> Vec<TaskEvent> {
        let Ok(mut receiver) = self.events.lock() else {
            return Vec::new();
        };
        let mut events = Vec::new();
        while let Ok(event) = receiver.try_recv() {
            events.push(event);
        }
        events
    }
}

/// Snapshot da aplicação usado pela apresentação. O valor padrão é simulado.
#[derive(Debug, Clone, PartialEq)]
pub struct DesktopState {
    pub eventos: Vec<TaskEvent>,
    pub fila_pausada: bool,
    pub diretorio_saida: String,
}

impl Default for DesktopState {
    fn default() -> Self {
        Self {
            eventos: vec![TaskEvent {
                task_id: "simulado-1".into(),
                sequence: 0,
                status: TaskStatus::NaFila,
                stage: None,
                progress: Some(0.0),
                speed: None,
                eta_seconds: None,
                message: Some("Pronto para iniciar".into()),
                destination: Some("~/Downloads".into()),
            }],
            fila_pausada: false,
            diretorio_saida: "~/Downloads".into(),
        }
    }
}

pub struct DownloaderApp {
    pub tela: Screen,
    pub estado: DesktopState,
    pub eventos: Vec<UiEvent>,
    url: String,
    download_mode: String,
    download_quality: String,
    download_format: String,
    download_bitrate: String,
    baixar_playlist: bool,
    arquivo_cookies: String,
    perfil_navegador: String,
    arquivo_conversao: String,
    formato: String,
    aceleracao: AccelerationMode,
    protecao_youtube: bool,
    diretorio: String,
    mensagem: String,
    next_id: u64,
    pub runtime: Option<DesktopRuntime>,
}

impl Default for DownloaderApp {
    fn default() -> Self {
        let estado = DesktopState::default();
        Self {
            tela: Screen::Downloads,
            diretorio: estado.diretorio_saida.clone(),
            estado,
            eventos: Vec::new(),
            url: String::new(),
            download_mode: "video_audio".into(),
            download_quality: "best".into(),
            download_format: "auto".into(),
            download_bitrate: "192k".into(),
            baixar_playlist: false,
            arquivo_cookies: String::new(),
            perfil_navegador: String::new(),
            arquivo_conversao: String::new(),
            formato: "mp4".into(),
            aceleracao: AccelerationMode::Auto,
            protecao_youtube: true,
            mensagem: "Dados simulados — conecte a camada de aplicação para operar downloads."
                .into(),
            next_id: 1,
            runtime: None,
        }
    }
}

impl DownloaderApp {
    pub fn with_runtime(mut self, runtime: DesktopRuntime) -> Self {
        self.runtime = Some(runtime);
        self.estado.eventos.clear();
        self.mensagem = "Pronto. Escolha uma URL ou um arquivo para começar.".into();
        self
    }

    /// Adapta um evento de comando para a aplicação. Eventos de apresentação
    /// (navegação e pausa) ficam sob responsabilidade do integrador futuro.
    pub fn dispatch_event(
        application: &mut ApplicationState,
        event: UiEvent,
    ) -> Result<Vec<TaskEvent>, AppError> {
        match event {
            UiEvent::Comando(command) => application.dispatch(command),
            UiEvent::Navegar(_) | UiEvent::PausarFila | UiEvent::RetomarFila => Ok(Vec::new()),
        }
    }

    /// Constrói o registro de domínio que será enviado via `AppCommand::Enqueue`.
    fn new_task(
        &mut self,
        kind: TaskKind,
        url: Option<String>,
        path: Option<String>,
        options: serde_json::Value,
        auth: Option<AuthRef>,
    ) -> TaskRecord {
        let id = format!(
            "ui-{}-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |duration| duration.as_millis()),
            self.next_id
        );
        self.next_id += 1;
        TaskRecord {
            id,
            kind,
            input_url: url,
            input_path: path,
            options,
            destination: Some(self.estado.diretorio_saida.clone()),
            collision: CollisionPolicy::Rename,
            auth,
            status: TaskStatus::NaFila,
            temporary_path: None,
            error: None,
            created_at_ms: 0,
            updated_at_ms: 0,
        }
    }

    fn emit(&mut self, event: UiEvent) {
        if let UiEvent::Comando(command) = &event {
            if let Some(runtime) = &self.runtime {
                let _ = runtime.send(command.clone());
            }
        }
        self.eventos.push(event);
    }

    fn selected_auth(&self) -> Option<AuthRef> {
        if !self.arquivo_cookies.trim().is_empty() {
            return Some(AuthRef {
                id: self.arquivo_cookies.trim().into(),
                kind: AuthKind::CookieFile,
            });
        }
        if !self.perfil_navegador.trim().is_empty() {
            return Some(AuthRef {
                id: self.perfil_navegador.trim().into(),
                kind: AuthKind::BrowserProfile,
            });
        }
        // Sem preenchimento manual, o runtime tenta o primeiro perfil local
        // conhecido e continua sem autenticação se nenhum existir.
        Some(AuthRef {
            id: "auto".into(),
            kind: AuthKind::BrowserProfile,
        })
    }
    fn navigation(&mut self, ui: &mut egui::Ui) {
        ui.add_space(10.0);
        ui.label(
            egui::RichText::new("D / DOWNLOADER")
                .size(17.0)
                .strong()
                .color(ACCENT),
        );
        ui.add_space(4.0);
        ui.weak("Sua biblioteca de mídia");
        ui.add_space(34.0);
        for (screen, label) in [
            (Screen::Downloads, "Downloads"),
            (Screen::Conversor, "Conversor"),
            (Screen::FilaHistorico, "Fila e histórico"),
            (Screen::Configuracoes, "Configurações"),
        ] {
            let selected = self.tela == screen;
            let button =
                egui::Button::new(egui::RichText::new(label).size(15.0).color(if selected {
                    ACCENT
                } else {
                    TEXT
                }))
                .fill(if selected {
                    egui::Color32::from_rgb(28, 55, 58)
                } else {
                    egui::Color32::TRANSPARENT
                })
                .stroke(egui::Stroke::NONE)
                .rounding(8.0);
            if ui.add_sized([ui.available_width(), 44.0], button).clicked() {
                self.tela = screen;
                self.emit(UiEvent::Navegar(screen));
            }
            ui.add_space(5.0);
        }
        ui.add_space(32.0);
        ui.separator();
        ui.add_space(10.0);
        ui.small("SALVAR EM");
        ui.label(
            egui::RichText::new(&self.estado.diretorio_saida)
                .size(12.0)
                .color(MUTED),
        );
    }

    fn downloads(&mut self, ui: &mut egui::Ui) {
        page_title(
            ui,
            "Sua próxima descoberta.",
            "Baixe vídeos, músicas ou uma playlist inteira.",
        );
        card(ui, |ui| {
            section_title(ui, "01", "Link do vídeo ou playlist");
            ui.add_sized(
                [ui.available_width(), 40.0],
                egui::TextEdit::singleline(&mut self.url)
                    .hint_text("Cole o link aqui…")
                    .margin(egui::vec2(12.0, 10.0)),
            );
            ui.add_space(6.0);
            ui.checkbox(&mut self.baixar_playlist, "Baixar a playlist inteira");
        });
        ui.add_space(10.0);
        card(ui, |ui| {
            section_title(ui, "02", "Como você quer salvar?");
            egui::Grid::new("download-options")
                .num_columns(4)
                .spacing([18.0, 14.0])
                .show(ui, |ui| {
                    ui.label("Conteúdo");
                    choice(
                        ui,
                        "download-mode",
                        &mut self.download_mode,
                        &[
                            ("video_audio", "Vídeo + áudio"),
                            ("audio", "Somente áudio"),
                            ("video", "Somente vídeo"),
                        ],
                    );
                    ui.label("Formato");
                    choice(
                        ui,
                        "download-format",
                        &mut self.download_format,
                        &[
                            ("auto", "Automático"),
                            ("mp4", "MP4"),
                            ("mkv", "MKV"),
                            ("webm", "WebM"),
                            ("mp3", "MP3 · áudio"),
                            ("opus", "Opus · áudio"),
                        ],
                    );
                    ui.end_row();
                    ui.label("Resolução");
                    choice(
                        ui,
                        "download-quality",
                        &mut self.download_quality,
                        &[
                            ("best", "Melhor disponível"),
                            ("2160", "Até 2160p"),
                            ("1440", "Até 1440p"),
                            ("1080", "Até 1080p"),
                            ("720", "Até 720p"),
                            ("480", "Até 480p"),
                        ],
                    );
                    ui.label("Taxa de bits");
                    choice(
                        ui,
                        "download-bitrate",
                        &mut self.download_bitrate,
                        &[
                            ("auto", "Automática"),
                            ("64k", "64 kbps"),
                            ("96k", "96 kbps"),
                            ("128k", "128 kbps"),
                            ("192k", "192 kbps"),
                            ("256k", "256 kbps"),
                            ("320k", "320 kbps"),
                        ],
                    );
                    ui.end_row();
                });
            ui.add_space(12.0);
            ui.separator();
            ui.add_space(6.0);
            ui.label(
                egui::RichText::new("Capa automática  ·  Nome pelo título  ·  Evita duplicatas")
                    .size(12.0)
                    .color(ACCENT),
            );
            ui.weak("Opus e MP3 salvam somente áudio. A capa é incluída quando disponível.");
        });
        ui.add_space(10.0);
        card(ui, |ui| {
            egui::CollapsingHeader::new("Acesso e cookies · automático").show(ui, |ui| {
                ui.weak(
                    "Usamos um perfil local compatível. Preencha abaixo para escolher outra fonte.",
                );
                ui.label("Arquivo de cookies");
                ui.add(
                    egui::TextEdit::singleline(&mut self.arquivo_cookies)
                        .hint_text("/caminho/cookies.txt")
                        .desired_width(f32::INFINITY),
                );
                ui.label("Perfil do navegador");
                ui.add(
                    egui::TextEdit::singleline(&mut self.perfil_navegador)
                        .hint_text("Ex.: firefox ou chrome:Default")
                        .desired_width(f32::INFINITY),
                );
            });
        });
        ui.add_space(18.0);
        ui.horizontal_wrapped(|ui| {
            if ui
                .add_enabled(
                    !self.url.trim().is_empty(),
                    egui::Button::new("Consultar informações"),
                )
                .clicked()
            {
                self.emit(UiEvent::Comando(AppCommand::Analyze {
                    url: self.url.trim().into(),
                    auth: self.selected_auth(),
                    playlist: self.baixar_playlist,
                }));
                self.mensagem = "Consultando informações do vídeo…".into();
            }
            if ui
                .add_enabled(
                    !self.url.trim().is_empty(),
                    primary_button("Adicionar à fila"),
                )
                .clicked()
            {
                let options = serde_json::json!({
                    "mode": self.download_mode,
                    "quality": self.download_quality,
                    "output_format": self.download_format,
                    "playlist": self.baixar_playlist,
                    "bitrate": self.download_bitrate,
                });
                let auth = self.selected_auth();
                let task = self.new_task(
                    TaskKind::Download,
                    Some(self.url.trim().into()),
                    None,
                    options,
                    auth,
                );
                self.emit(UiEvent::Comando(AppCommand::Enqueue(Box::new(task))));
                self.mensagem = "Download enviado para a fila.".into();
                self.url.clear();
            }
        });
    }
    fn converter(&mut self, ui: &mut egui::Ui) {
        page_title(
            ui,
            "Um arquivo. Novas possibilidades.",
            "Converta sua mídia no formato que combina com você.",
        );
        card(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label("Arquivo:");
                ui.add(
                    egui::TextEdit::singleline(&mut self.arquivo_conversao)
                        .hint_text("Caminho do arquivo de mídia")
                        .desired_width(430.0),
                );
            });
            ui.horizontal(|ui| {
                ui.label("Formato:");
                egui::ComboBox::from_id_salt("formato")
                    .selected_text(&self.formato)
                    .show_ui(ui, |ui| {
                        for formato in ["mp4", "mkv", "mp3", "webm"] {
                            ui.selectable_value(&mut self.formato, formato.into(), formato);
                        }
                    });
                ui.label("Aceleração:");
                egui::ComboBox::from_id_salt("aceleracao")
                    .selected_text(acceleration_label(self.aceleracao))
                    .show_ui(ui, |ui| {
                        for mode in [
                            AccelerationMode::Auto,
                            AccelerationMode::Software,
                            AccelerationMode::Vulkan,
                            AccelerationMode::Vaapi,
                            AccelerationMode::Amf,
                            AccelerationMode::Cuda,
                            AccelerationMode::Qsv,
                        ] {
                            ui.selectable_value(
                                &mut self.aceleracao,
                                mode,
                                acceleration_label(mode),
                            );
                        }
                    });
                if ui.add(primary_button("Iniciar conversão")).clicked()
                    && !self.arquivo_conversao.trim().is_empty()
                {
                    let task = self.new_task(
                        TaskKind::Convert,
                        None,
                        Some(self.arquivo_conversao.trim().into()),
                        serde_json::json!({
                            "formato": self.formato,
                            "aceleracao": acceleration_name(self.aceleracao),
                        }),
                        None,
                    );
                    self.emit(UiEvent::Comando(AppCommand::Enqueue(Box::new(task))));
                    self.mensagem = "Conversão enviada para a aplicação.".into();
                }
            });
        });
    }
    fn queue(&mut self, ui: &mut egui::Ui) {
        page_title(
            ui,
            "Cada faixa, no seu ritmo.",
            "Acompanhe transferências, conversões e arquivos concluídos.",
        );
        let active = self
            .estado
            .eventos
            .iter()
            .filter(|e| !e.status.terminal())
            .count();
        let done = self
            .estado
            .eventos
            .iter()
            .filter(|e| e.status == TaskStatus::Concluido)
            .count();
        let failed = self
            .estado
            .eventos
            .iter()
            .filter(|e| e.status == TaskStatus::Falhou)
            .count();
        ui.horizontal_wrapped(|ui| {
            for (label, value, color) in [
                ("Em andamento", active, ACCENT),
                ("Concluídos", done, egui::Color32::from_rgb(92, 196, 122)),
                ("Falhas", failed, egui::Color32::from_rgb(232, 105, 105)),
            ] {
                egui::Frame::none()
                    .fill(SURFACE)
                    .rounding(10.0)
                    .inner_margin(16.0)
                    .show(ui, |ui| {
                        ui.set_min_width(120.0);
                        ui.label(
                            egui::RichText::new(value.to_string())
                                .size(26.0)
                                .strong()
                                .color(color),
                        );
                        ui.weak(label);
                    });
            }
        });
        ui.add_space(20.0);
        if self.estado.eventos.is_empty() {
            card(ui, |ui| {
                ui.add_space(20.0);
                ui.heading("Sua fila está vazia");
                ui.weak("Adicione um link em Downloads para começar.");
                if ui.button("Ir para Downloads").clicked() {
                    self.tela = Screen::Downloads;
                }
                ui.add_space(20.0);
            });
        }
        for item in self.estado.eventos.clone() {
            ui.push_id(&item.task_id, |ui| {
                card(ui, |ui| {
                    ui.horizontal_wrapped(|ui| {
                        ui.colored_label(
                            status_color(&item.status),
                            egui::RichText::new(status_label(&item.status))
                                .small()
                                .strong(),
                        );
                        if !item.status.terminal() && ui.small_button("Cancelar").clicked() {
                            self.emit(UiEvent::Comando(AppCommand::Cancel {
                                task_id: item
                                    .task_id
                                    .split("::")
                                    .next()
                                    .unwrap_or(&item.task_id)
                                    .to_owned(),
                            }));
                        }
                    });
                    ui.label(
                        egui::RichText::new(
                            item.message
                                .as_deref()
                                .filter(|s| !s.is_empty())
                                .unwrap_or("Tarefa de mídia"),
                        )
                        .size(16.0)
                        .strong(),
                    );
                    if let Some(progress) = item.progress {
                        ui.add(
                            egui::ProgressBar::new(progress.clamp(0.0, 1.0))
                                .fill(status_color(&item.status))
                                .desired_height(7.0),
                        );
                        ui.small(format!("{:.0}%", progress.clamp(0.0, 1.0) * 100.0));
                    } else if !item.status.terminal() {
                        ui.horizontal(|ui| {
                            ui.spinner();
                            ui.weak(if item.status == TaskStatus::NaFila {
                                "Aguardando liberação…"
                            } else {
                                "Processando…"
                            });
                        });
                    }
                    if let Some(speed) = &item.speed {
                        ui.small(format!("Velocidade: {speed}"));
                    }
                    if let Some(path) = &item.destination {
                        ui.label(egui::RichText::new(path).size(12.0).color(MUTED));
                    }
                })
            });
            ui.add_space(10.0);
        }
    }
    fn settings(&mut self, ui: &mut egui::Ui) {
        page_title(
            ui,
            "Do seu jeito.",
            "Escolha onde sua biblioteca será salva.",
        );
        card(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label("Diretório de saída:");
                ui.add(egui::TextEdit::singleline(&mut self.diretorio).desired_width(360.0));
                if ui.add(primary_button("Salvar pasta")).clicked() {
                    self.estado.diretorio_saida = self.diretorio.clone();
                    self.emit(UiEvent::Comando(AppCommand::UpdateSettings {
                        values: serde_json::json!({"diretorio_saida": self.diretorio}),
                    }));
                    self.mensagem = "Pasta de destino atualizada.".into();
                }
            });
        });
        ui.add_space(14.0);
        card(ui, |ui| {
            section_title(ui, "YT", "Proteção do YouTube");
            let changed = ui
                .checkbox(&mut self.protecao_youtube, "Ativar limite de segurança")
                .changed();
            if changed {
                self.emit(UiEvent::Comando(AppCommand::UpdateSettings {
                    values: serde_json::json!({"youtube_protecao": self.protecao_youtube}),
                }));
                self.mensagem = if self.protecao_youtube {
                    "Proteção do YouTube ativada.".into()
                } else {
                    "Proteção do YouTube desativada.".into()
                };
            }
            if self.protecao_youtube {
                ui.label("5 segundos entre downloads · até 300 vídeos em 90 minutos");
                ui.weak(
                    "A contagem continua após reiniciar. A fila aguarda quando o limite é atingido.",
                );
                ui.weak("Falhas temporárias: até 3 novas tentativas, com espera progressiva.");
            } else {
                ui.weak("O yt-dlp usará seu comportamento padrão para URLs do YouTube.");
            }
        });
    }
}

const BACKGROUND: egui::Color32 = egui::Color32::from_rgb(12, 18, 26);
const SURFACE: egui::Color32 = egui::Color32::from_rgb(23, 32, 43);
const ACCENT: egui::Color32 = egui::Color32::from_rgb(104, 222, 195);
const TEXT: egui::Color32 = egui::Color32::from_rgb(232, 239, 244);
const MUTED: egui::Color32 = egui::Color32::from_rgb(151, 169, 184);

fn apply_theme(ctx: &egui::Context) {
    let mut style = (*ctx.style()).clone();
    style.spacing.item_spacing = egui::vec2(10.0, 10.0);
    style.spacing.button_padding = egui::vec2(16.0, 10.0);
    style.spacing.interact_size.y = 34.0;
    style.visuals = egui::Visuals::dark();
    style.visuals.override_text_color = Some(TEXT);
    style.visuals.panel_fill = BACKGROUND;
    style.visuals.extreme_bg_color = BACKGROUND;
    style.visuals.selection.bg_fill = egui::Color32::from_rgb(34, 83, 78);
    style.visuals.selection.stroke = egui::Stroke::new(1.0_f32, ACCENT);
    style.visuals.widgets.inactive.bg_fill = egui::Color32::from_rgb(33, 46, 60);
    style.visuals.widgets.inactive.weak_bg_fill = egui::Color32::from_rgb(33, 46, 60);
    style.visuals.widgets.inactive.bg_stroke =
        egui::Stroke::new(1.0_f32, egui::Color32::from_rgb(49, 64, 79));
    style.visuals.widgets.hovered.bg_fill = egui::Color32::from_rgb(43, 65, 78);
    style.visuals.widgets.hovered.weak_bg_fill = egui::Color32::from_rgb(43, 65, 78);
    for widget in [
        &mut style.visuals.widgets.inactive,
        &mut style.visuals.widgets.hovered,
        &mut style.visuals.widgets.active,
    ] {
        widget.rounding = egui::Rounding::same(7.0);
    }
    style
        .text_styles
        .insert(egui::TextStyle::Body, egui::FontId::proportional(14.0));
    style
        .text_styles
        .insert(egui::TextStyle::Button, egui::FontId::proportional(14.0));
    ctx.set_style(style);
}
fn card(ui: &mut egui::Ui, content: impl FnOnce(&mut egui::Ui)) {
    egui::Frame::none()
        .fill(SURFACE)
        .rounding(12.0)
        .stroke(egui::Stroke::new(
            1.0_f32,
            egui::Color32::from_rgb(35, 47, 61),
        ))
        .inner_margin(16.0)
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            content(ui);
        });
}
fn primary_button(label: &str) -> egui::Button<'_> {
    egui::Button::new(egui::RichText::new(label).strong().color(BACKGROUND))
        .fill(ACCENT)
        .min_size(egui::vec2(160.0, 40.0))
}
fn page_title(ui: &mut egui::Ui, title: &str, subtitle: &str) {
    ui.label(egui::RichText::new(title).size(28.0).strong());
    ui.label(egui::RichText::new(subtitle).color(MUTED));
    ui.add_space(16.0);
}
fn section_title(ui: &mut egui::Ui, number: &str, title: &str) {
    ui.horizontal(|ui| {
        ui.label(
            egui::RichText::new(number)
                .size(12.0)
                .strong()
                .color(ACCENT),
        );
        ui.label(egui::RichText::new(title).size(16.0).strong());
    });
    ui.add_space(8.0);
}
fn choice(ui: &mut egui::Ui, id: &str, value: &mut String, options: &[(&str, &str)]) {
    let label = options
        .iter()
        .find(|(key, _)| *key == value)
        .map(|(_, label)| *label)
        .unwrap_or(value);
    egui::ComboBox::from_id_salt(id)
        .width(155.0)
        .selected_text(label)
        .show_ui(ui, |ui| {
            for (key, label) in options {
                ui.selectable_value(value, (*key).into(), *label);
            }
        });
}

fn acceleration_name(mode: AccelerationMode) -> &'static str {
    match mode {
        AccelerationMode::Auto => "auto",
        AccelerationMode::Software => "software",
        AccelerationMode::Vulkan => "vulkan",
        AccelerationMode::Vaapi => "vaapi",
        AccelerationMode::Amf => "amf",
        AccelerationMode::Cuda => "cuda",
        AccelerationMode::Qsv => "qsv",
    }
}

fn acceleration_label(mode: AccelerationMode) -> &'static str {
    match mode {
        AccelerationMode::Auto => "Automático",
        AccelerationMode::Software => "Software",
        AccelerationMode::Vulkan => "Vulkan",
        AccelerationMode::Vaapi => "VAAPI",
        AccelerationMode::Amf => "AMF",
        AccelerationMode::Cuda => "CUDA/NVENC",
        AccelerationMode::Qsv => "Intel QSV",
    }
}

fn status_label(status: &TaskStatus) -> &'static str {
    match status {
        TaskStatus::NaFila => "Na fila",
        TaskStatus::Analisando => "Analisando",
        TaskStatus::AguardandoAutenticacao => "Aguardando autenticação",
        TaskStatus::Baixando => "Baixando",
        TaskStatus::Unindo => "Unindo",
        TaskStatus::Convertendo => "Convertendo",
        TaskStatus::Validando => "Validando",
        TaskStatus::Concluido => "Concluído",
        TaskStatus::Falhou => "Falhou",
        TaskStatus::Cancelado => "Cancelado",
        TaskStatus::Interrompido => "Interrompido",
    }
}

fn status_color(status: &TaskStatus) -> egui::Color32 {
    match status {
        TaskStatus::Concluido => egui::Color32::from_rgb(92, 196, 122),
        TaskStatus::Falhou | TaskStatus::Cancelado => egui::Color32::from_rgb(232, 105, 105),
        TaskStatus::Baixando | TaskStatus::Convertendo | TaskStatus::Analisando => {
            egui::Color32::from_rgb(103, 169, 255)
        }
        _ => egui::Color32::from_rgb(210, 180, 90),
    }
}

impl DownloaderApp {
    /// Draw the interface independently of the native window for previews.
    pub fn draw(&mut self, ctx: &egui::Context) {
        ctx.request_repaint_after(std::time::Duration::from_millis(100));
        apply_theme(ctx);
        if let Some(runtime) = &self.runtime {
            for event in runtime.poll_events() {
                if let Some(message) = &event.message {
                    self.mensagem = message.clone();
                }
                if let Some(current) = self
                    .estado
                    .eventos
                    .iter_mut()
                    .find(|current| current.task_id == event.task_id)
                {
                    *current = event;
                } else {
                    self.estado.eventos.push(event);
                }
            }
        }
        egui::SidePanel::left("navegacao")
            .exact_width(202.0)
            .resizable(false)
            .frame(
                egui::Frame::none()
                    .fill(egui::Color32::from_rgb(16, 23, 32))
                    .inner_margin(18.0),
            )
            .show(ctx, |ui| self.navigation(ui));
        egui::TopBottomPanel::bottom("status")
            .frame(
                egui::Frame::none()
                    .fill(BACKGROUND)
                    .inner_margin(egui::Margin::symmetric(24.0, 12.0)),
            )
            .show(ctx, |ui| {
                ui.horizontal_wrapped(|ui| {
                    let (rect, _) =
                        ui.allocate_exact_size(egui::vec2(8.0, 8.0), egui::Sense::hover());
                    ui.painter().circle_filled(rect.center(), 3.0, ACCENT);
                    ui.label(egui::RichText::new(&self.mensagem).size(12.0).color(MUTED));
                });
            });
        egui::CentralPanel::default()
            .frame(egui::Frame::none().fill(BACKGROUND).inner_margin(28.0))
            .show(ctx, |ui| {
                egui::ScrollArea::vertical()
                    .id_salt(self.tela.label())
                    .show(ui, |ui| {
                        ui.set_max_width(ui.available_width().min(1000.0));
                        match self.tela {
                            Screen::Downloads => self.downloads(ui),
                            Screen::Conversor => self.converter(ui),
                            Screen::FilaHistorico => self.queue(ui),
                            Screen::Configuracoes => self.settings(ui),
                        }
                    });
            });
    }
}

impl eframe::App for DownloaderApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.draw(ctx);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn navegacao_tem_quatro_telas_estaveis() {
        assert_eq!(Screen::ALL.len(), 4);
        assert_eq!(Screen::ALL[2].label(), "Fila / Histórico");
    }
    #[test]
    fn estado_inicial_e_simulado_em_portugues() {
        let app = DownloaderApp::default();
        assert_eq!(app.tela, Screen::Downloads);
        assert_eq!(app.estado.eventos.len(), 1);
        assert!(app.mensagem.contains("simulados"));
    }
    #[test]
    fn url_vira_comando_de_dominio() {
        let mut app = DownloaderApp::default();
        let task = app.new_task(
            TaskKind::Download,
            Some("https://exemplo.test/video".into()),
            None,
            serde_json::json!({}),
            None,
        );
        app.emit(UiEvent::Comando(AppCommand::Enqueue(Box::new(task))));
        assert!(matches!(
            app.eventos[0],
            UiEvent::Comando(AppCommand::Enqueue(_))
        ));
    }
    #[test]
    fn status_tem_rotulo_legivel() {
        assert_eq!(
            status_label(&TaskStatus::AguardandoAutenticacao),
            "Aguardando autenticação"
        );
        assert_eq!(
            status_color(&TaskStatus::Concluido),
            egui::Color32::from_rgb(92, 196, 122)
        );
    }
    #[test]
    fn cancelamento_usa_task_id_do_dominio() {
        let event = UiEvent::Comando(AppCommand::Cancel {
            task_id: "abc".into(),
        });
        assert!(
            matches!(event, UiEvent::Comando(AppCommand::Cancel { task_id }) if task_id == "abc")
        );
    }

    #[test]
    fn aceleracao_tem_modo_automatico_e_nome_estavel() {
        let app = DownloaderApp::default();
        assert_eq!(app.aceleracao, AccelerationMode::Auto);
        assert_eq!(acceleration_name(AccelerationMode::Vulkan), "vulkan");
        assert_eq!(acceleration_label(AccelerationMode::Software), "Software");
    }
    #[test]
    fn cookies_da_ui_viram_referencia_sem_conteudo_secreto() {
        let app = DownloaderApp {
            arquivo_cookies: "/tmp/meus cookies.txt".into(),
            ..DownloaderApp::default()
        };
        let auth = app.selected_auth().expect("cookie selecionado");
        assert_eq!(auth.kind, AuthKind::CookieFile);
        assert_eq!(auth.id, "/tmp/meus cookies.txt");
        assert!(!serde_json::to_string(&auth)
            .expect("serializar auth")
            .contains("secret"));
    }
    #[test]
    fn adaptador_encaminha_comando_para_application() {
        let mut application = ApplicationState::new(downloader_application::QueueLimits::default());
        let event = UiEvent::Comando(AppCommand::ListTasks);
        assert!(DownloaderApp::dispatch_event(&mut application, event).is_ok());
    }

    #[test]
    fn runtime_retorna_evento_da_fila() {
        let base = std::env::temp_dir().join(format!(
            "downloader-ui-test-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("clock")
                .as_nanos()
        ));
        let runtime = DesktopRuntime::spawn_with_config(
            downloader_application::EngineConfig {
                data_dir: base.join("state"),
                output_dir: base.join("downloads"),
                ..downloader_application::EngineConfig::default()
            },
            downloader_application::QueueLimits::default(),
        );
        let task_id = format!(
            "runtime-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("clock")
                .as_nanos()
        );
        let task = TaskRecord {
            id: task_id.clone(),
            kind: TaskKind::Download,
            input_url: Some("https://exemplo.test/video".into()),
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
        };
        runtime
            .send(AppCommand::Enqueue(Box::new(task)))
            .expect("canal aberto");
        let mut events = Vec::new();
        for _ in 0..20 {
            events = runtime.poll_events();
            if !events.is_empty() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert_eq!(
            events.first().map(|event| event.task_id.as_str()),
            Some(task_id.as_str()),
            "eventos: {events:?}"
        );
    }
}
