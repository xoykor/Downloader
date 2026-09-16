# Downloader

A implementação entregue está em Rust. O protótipo Python legado foi retirado
do workspace durante a limpeza final; o histórico da migração está no handoff.

## Executar

```bash
cd rust
cargo build --release --locked
./target/release/downloader-desktop
```

Em ambiente sem compositor gráfico, use a CLI:

```bash
./target/release/downloader-cli analyze <url> [--playlist]
./target/release/downloader-cli download <url> [diretório] [--playlist]
./target/release/downloader-cli convert <arquivo> [mp4|mkv|mp3|webm] [diretório]
```

Para conteúdo que exige sessão, a interface tenta automaticamente o primeiro
perfil local compatível. Também é possível informar um `cookies.txt` no formato
Netscape (`--cookies /caminho/cookies.txt`) ou um perfil (`--browser
firefox[:perfil]`) nos comandos `analyze` e `download`.

## Abrir com dois cliques (Linux)

Compile e instale o atalho do aplicativo no menu do usuário:

```bash
cd /home/x/Documentos/Estudo/Downloader
./packaging/install-desktop.sh
```

Depois, procure por **Downloader** no menu de aplicativos. Também é possível
copiar [`packaging/Downloader.desktop`](packaging/Downloader.desktop) para a
área de trabalho e abrir diretamente; o gerenciador pode pedir **Permitir
iniciar** na primeira execução.

Na tela Downloads é possível escolher vídeo + áudio, somente vídeo ou áudio,
qualidade de 480p a 2160p, formato MP4/MKV/WebM/MP3/Opus, taxa de bits e playlist.
Downloads usam o título do vídeo no nome do arquivo, incorporam a thumbnail
como capa nos formatos compatíveis e mostram progresso separado por faixa.
O programa requer `yt-dlp`, `ffmpeg` e `ffprobe` no `PATH`. A fila é salva em
`~/.downloader/tasks.sqlite`; arquivos temporários só são publicados depois da
validação com `ffprobe`. Aceleração Vulkan/VAAPI/AMF/CUDA/QSV é usada apenas
quando a sondagem do dispositivo passa; caso contrário, a conversão recua para
software.

## Verificação

```bash
cd rust
cargo test --workspace --locked
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
```

O estado detalhado, limites conhecidos, autenticação e o roteiro dos quatro agentes estão em
[`docs/HANDOFF-estado-e-conclusao-2026-09-15.md`](docs/HANDOFF-estado-e-conclusao-2026-09-15.md).

Para entender os módulos e os fluxos, consulte [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
e o [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md).

## Proteção do YouTube

Pausa de 5 segundos entre downloads e até 300 inícios de vídeo por janela móvel
de 90 minutos, com contagem persistente e espera automática. A proteção pode ser
ativada ou desativada em Configurações (vem ativada por padrão). Falhas
temporárias recebem até três novas tentativas. Veja [limites e fontes](docs/YOUTUBE_LIMITS.md).
