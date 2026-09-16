# Downloader Desktop

Crate de interface desktop em `egui/eframe`, com navegação permanente entre Downloads, Conversor, Fila / Histórico e Configurações. A UI emite `AppCommand` do `downloader-domain` embrulhado em `UiEvent::Comando` e recebe eventos `TaskEvent` em `DesktopState`. `DownloaderApp::default()` permanece útil para testes; o binário inicia com `DesktopRuntime::spawn()`, que executa yt-dlp/ffprobe/ffmpeg em uma thread Tokio separada.

`DesktopRuntime::spawn()` oferece o adaptador pronto: uma thread Tokio mantém a fila persistida, recebe `AppCommand` por canal e devolve `TaskEvent`. Construa a UI com `DownloaderApp::default().with_runtime(runtime)`; a UI continua responsável apenas pela apresentação.

Na tela Downloads, os comandos carregam tipo de mídia, limite de qualidade,
formato de saída, taxa de bits e a opção explícita de baixar a playlist inteira.
Também há autenticação opcional por arquivo Netscape (`cookies.txt`) ou perfil
de navegador (`firefox[:perfil]`); somente a referência é colocada no comando.

## Validação local

Validação executada no workspace:

```text
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
```

## Contrato para integração

- O runtime já consome os comandos emitidos por `DownloaderApp::eventos`.
- Downloads e conversões são enviados como `AppCommand::Enqueue(Box<TaskRecord>)`; a aplicação é responsável por validação completa, fila e persistência.
- Cancelamentos são `AppCommand::Cancel { task_id }`, usando o `TaskId` do domínio.
- `PausarFila` e `RetomarFila` ainda são eventos de apresentação até existir comando equivalente na aplicação.
- Preferências são `AppCommand::UpdateSettings` com JSON; a aplicação deve validar e persistir o caminho.
- A conversão inclui a opção `aceleracao` (`auto`, `software`, `vulkan`, `vaapi`, `amf`, `cuda` ou `qsv`); a aplicação deve cruzá-la com a sondagem de `infra::hardware` antes de executar.
- O runtime publica `TaskEvent` reais; a UI mantém somente o último evento de cada tarefa para a fila.
- Downloads, análise e conversões locais já são executados pelos workers de mídia/processos; cancelamento envia `CancellationToken` ao processo em andamento.
