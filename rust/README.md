# Reconstrução Rust do Downloader

Esta pasta contém a implementação compilada do aplicativo. O protótipo Python
legado foi retirado do workspace após a validação da reconstrução; o histórico
da transição está em `../docs/HANDOFF-estado-e-conclusao-2026-09-15.md`.

## Estrutura

- `crates/domain`: contratos, estados e erros compartilhados.
- `crates/application`: fila, limites de concorrência e transições.
- `crates/infra`: processos externos, SQLite, autenticação e publicação.
- `crates/media`: parsers e planejamento de yt-dlp/ffprobe/FFmpeg.
- `crates/desktop`: interface egui/eframe ligada ao runtime assíncrono.
- `crates/cli`: executável headless para automatizar os mesmos fluxos.

## Desenvolvimento

Na máquina de desenvolvimento, execute a partir desta pasta:

```text
cargo fmt --all
cargo test --manifest-path crates/domain/Cargo.toml
cargo test --manifest-path crates/application/Cargo.toml
cargo test --manifest-path crates/infra/Cargo.toml
cargo test --manifest-path crates/media/Cargo.toml
cargo test --manifest-path crates/cli/Cargo.toml
cargo test --workspace --locked
cargo clippy --workspace --all-targets -- -D warnings
```

## Usar o programa

Compile em modo release e execute a interface:

```text
cargo build --release --locked
./target/release/downloader-desktop
```

Para servidores, testes ou ambientes sem compositor gráfico, o mesmo runtime
está disponível na CLI:

```text
./target/release/downloader-cli analyze <url>
./target/release/downloader-cli analyze <url> [--cookies arquivo | --browser navegador[:perfil]]
./target/release/downloader-cli download <url> [diretório] [--playlist] [--cookies arquivo | --browser navegador[:perfil]]
./target/release/downloader-cli convert <arquivo> [mp4|mkv|mp3|webm] [diretório]
```

Downloads autenticados aceitam `--cookies /caminho/cookies.txt` (arquivo
Netscape compatível com yt-dlp) ou `--browser firefox[:perfil]`, tanto em
`analyze` quanto em `download`. A interface expõe as mesmas opções na tela
Downloads. O conteúdo dos cookies não é carregado no estado da tarefa.

Use `analyze URL --playlist` para consultar uma playlist com listagem plana e
retorno rápido; a extração completa dos formatos acontece somente no download.

Na tela Downloads é possível escolher tipo de mídia, qualidade, formato de
saída, bitrate, playlist e autenticação opcional por `cookies.txt` ou perfil do
navegador. Os arquivos temporários ficam no diretório de saída e só são
publicados depois que o processo termina com sucesso. O estado da fila fica em
`~/.downloader/tasks.sqlite` por padrão; a interface usa `~/Downloads` como
destino inicial.

O primeiro build pode exigir o download das dependências gráficas do crates.io.
Depois de compilado, o binário release é aberto pelo atalho em
`../packaging/Downloader.desktop`.

## Regras de integração

Cada crate mantém sua lógica e seus testes. A UI envia comandos; a aplicação
é a única camada que controla estados e limites; infraestrutura executa
processos e persiste dados; mídia constrói planos e valida artefatos. Caminhos
e argumentos devem permanecer estruturados, sem shell. Segredos não entram em
eventos, logs ou fixtures.

`infra::hardware` distingue backends anunciados pelo FFmpeg de dispositivos
realmente utilizáveis. Vulkan, VAAPI e AMF só são usados após uma sondagem
bem-sucedida; caso contrário, a aplicação recua automaticamente para software.

## Atalho de dois cliques

Na raiz do projeto, execute `./packaging/install-desktop.sh`. O instalador
coloca o binário release em `~/.local/bin` e registra o atalho **Downloader**
em `~/.local/share/applications`. O arquivo `packaging/Downloader.desktop`
também pode ser copiado para a área de trabalho.
