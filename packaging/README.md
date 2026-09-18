# Empacotamento

## Instalação local

```sh
./packaging/install-desktop.sh
```

O script compila a implementação C, instala os binários em `~/.local/bin`, o
atalho em `~/.local/share/applications` e o ícone no tema do usuário.

## AppImage

O workflow `appimage.yml` compila C17 + GTK4, executa os testes e cria
`Downloader-x86_64.AppImage`. O pacote contém:

- `downloader-desktop`;
- `downloader-cli`;
- `yt-dlp` standalone;
- FFmpeg e ffprobe;
- bibliotecas necessárias coletadas pelo linuxdeploy.

Tags `v*` publicam automaticamente o AppImage e seu SHA-256 em uma GitHub
Release.
