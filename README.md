# Downloader

Downloader de mídia para Linux escrito em **C17**, com interface **GTK4** e CLI. O projeto separa extração, fila, conversão, validação e persistência para manter o fluxo previsível mesmo em downloads longos ou playlists.
O aplicativo usa `yt-dlp` para extração/download e `ffmpeg`/`ffprobe` para
conversão e validação. Esses programas são executados diretamente com
`fork`/`exec`, nunca através de shell.

## Recursos

- downloads de vídeo + áudio, somente vídeo ou somente áudio;
- qualidade máxima de 480p a 2160p;
- MP4, MKV, WebM, MP3, Opus, M4A, FLAC e WAV;
- playlists;
- cookies Netscape e perfis de navegador suportados pelo yt-dlp;
- fila desktop com até 2 downloads e 1 conversão em paralelo;
- cancelamento de tarefas ativas ou ainda na fila;
- SQLite para histórico e estado;
- validação com ffprobe antes de publicar qualquer arquivo;
- política de colisão segura (renomear/substituir/pular);
- detecção e sondagem de Vulkan, VAAPI, AMF, CUDA/NVENC e QSV;
- fallback automático para software quando a aceleração falha;
- índice `.downloader-library.json` para evitar downloads repetidos;
- limite conservador persistente para YouTube: 5 s entre downloads e até 300
  inícios em uma janela móvel de 90 minutos, quando a proteção está ativada.

## Arquitetura

```text
GTK4 / CLI
   |
   v
fila de tarefas
   |------> yt-dlp        (extração e download)
   |------> ffmpeg        (conversão)
   |------> ffprobe       (validação)
   |
   v
SQLite + índice local de biblioteca
```

Os subprocessos são iniciados com `fork`/`exec`, sem passar URLs ou argumentos por um shell intermediário.

## Dependências de compilação

Em distribuições Arch/CachyOS:

```sh
sudo pacman -S --needed base-devel cmake pkgconf gtk4 json-c sqlite ffmpeg
```

`yt-dlp` é necessário em execução se você não usar o AppImage.

## Compilar

Os comandos abaixo funcionam normalmente em fish:

```sh
cmake -S c -B build/c -DCMAKE_BUILD_TYPE=Release
cmake --build build/c --parallel
ctest --test-dir build/c --output-on-failure
```

Binários:

```text
build/c/downloader-desktop
build/c/downloader-cli
```

## CLI

```text
downloader-cli deps
downloader-cli analyze <url> [--playlist]
downloader-cli download <url> [diretório] [opções]
downloader-cli convert <arquivo> [formato] [diretório] [--acceleration modo]
```

Use `downloader-cli --help` para a lista completa de opções.

## Dados

O estado fica por padrão em `~/.downloader/tasks.sqlite`. Para testes ou
instalações isoladas, use `DOWNLOADER_DATA_DIR`.

Arquivos baixados ou convertidos são produzidos em uma área temporária,
validados e só depois publicados no destino. Cookies nunca são copiados para o
banco; somente a referência ao arquivo/perfil é persistida.

## Código

A implementação principal está em [`c/`](c/). A arquitetura está documentada
em [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) e o uso em
[`docs/USER_GUIDE.md`](docs/USER_GUIDE.md).


## Uso responsável

Use o Downloader apenas para conteúdo que você tenha permissão para baixar. Plataformas podem impor limites, autenticação, DRM ou termos próprios; o projeto não tenta contornar DRM.

A proteção de ritmo para YouTube é conservadora e existe para reduzir rajadas de requisições, não para garantir ausência de bloqueios ou mudanças de política da plataforma.

## Licença

GNU General Public License v3.0. Consulte [LICENSE](LICENSE).
