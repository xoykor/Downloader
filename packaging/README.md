# Atalho de dois cliques

Para instalar o executável e o atalho no menu do Linux:

```bash
./packaging/install-desktop.sh
```

Depois, abra **Downloader** no menu de aplicativos. O instalador compila o
binário release se necessário e instala uma cópia em `~/.local/bin`.

O arquivo [`Downloader.desktop`](Downloader.desktop) também pode ser copiado
para a área de trabalho; ele aponta para o binário release deste workspace.

O atalho usa o binário em `rust/target/release/downloader-desktop`, já
compilado após a validação do projeto. Se o gerenciador de arquivos pedir,
selecione **Permitir iniciar** uma vez.

## AppImage

O workflow [`appimage.yml`](../.github/workflows/appimage.yml) cria um
`Downloader-x86_64.AppImage` a cada push e anexa o arquivo a uma GitHub Release
quando uma tag `v*` é publicada. O pacote inclui o executável Rust, o `yt-dlp`
standalone e FFmpeg/ffprobe. O `AppRun` coloca esses auxiliares no `PATH`, para
que o aplicativo funcione sem instalação adicional.

Para gerar localmente, instale `linuxdeploy` e `appimagetool` e execute:

```bash
APPIMAGE_OUTPUT_DIR="$PWD/dist" ./packaging/build-appimage.sh
```
