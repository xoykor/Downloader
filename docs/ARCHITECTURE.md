# Arquitetura

O aplicativo é um workspace Cargo em Rust. A interface e a CLI compartilham
o mesmo runtime de fila; nenhuma delas executa `yt-dlp`, `ffmpeg` ou `ffprobe`
diretamente.

```text
desktop (egui/eframe) ─┐
cli -------------------┼─> application (fila e estados)
                       │        │
                       │        ├─> media (planos e argumentos)
                       │        └─> infra (processos, SQLite, publicação, hardware)
                       └─> domain (contratos serializáveis)
```

## Crates

- **domain:** `TaskRecord`, `TaskEvent`, comandos, estados, erros, planos e
  referências de autenticação. Não acessa rede, disco ou processos.
- **application:** escalonador, limites de concorrência, recuperação de tarefas
  e execução das etapas. Publica somente artefatos validados.
- **media:** parsers de JSON, seletores de yt-dlp, planos de conversão,
  argumentos FFmpeg e validação de propriedades de mídia.
- **infra:** `TokioProcessRunner`, SQLite, autenticação, publicação atômica e
  sondagem de backends de hardware.
- **desktop:** telas Downloads, Conversor, Fila/Histórico e Configurações.
- **cli:** comandos headless para automação e diagnóstico.

## Fluxo de download

1. A UI/CLI cria um `TaskRecord` com URL, formato, qualidade, bitrate, playlist
   e autenticação opcional. A UI usa a referência `auto` quando nenhum campo
   manual foi preenchido.
2. `application` coloca a tarefa na fila e inicia o processo quando há limite.
3. `media` constrói argumentos separados para yt-dlp; não há shell.
4. O resultado fica em arquivo temporário oculto.
5. `ffprobe` valida cada arquivo e `infra::publish` aplica a política de colisão.
6. A aplicação registra destino e estado final no SQLite.

## Fluxo de conversão

1. `ffprobe` descreve streams e contêiner.
2. `media` escolhe cópia ou recodificação conforme o plano.
3. `infra::hardware` testa o backend solicitado; falha retorna ao software.
4. FFmpeg escreve em temporário, a saída é validada e só então publicada.

## Persistência e privacidade

O estado fica em `~/.downloader/tasks.sqlite` (com fallback local quando o HOME
é somente leitura). Registros guardam referências de autenticação, nunca o
conteúdo de cookies. O modo automático apenas verifica diretórios conhecidos e
deixa a leitura dos cookies para o yt-dlp no momento da execução. Logs, eventos
e mensagens de erro não devem conter credenciais ou cabeçalhos.

## Testes

Execute a partir de `rust/`:

```bash
cargo fmt --all -- --check
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
```

Testes que exigem rede ou uma GPU real ficam separados dos testes determinísticos
e não são necessários para compilar o aplicativo.
