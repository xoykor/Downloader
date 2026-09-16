#!/usr/bin/env bash
set -euo pipefail

project_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build_dir="${APPIMAGE_BUILD_DIR:-$project_dir/build/appimage}"
appdir="$build_dir/AppDir"
output_dir="${APPIMAGE_OUTPUT_DIR:-$project_dir/dist}"

rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" \
  "$appdir/usr/share/icons/hicolor/scalable/apps" "$output_dir"
rm -f "$output_dir"/*.AppImage "$output_dir"/*.AppImage.sha256

echo "Compilando Downloader em modo release..."
(cd "$project_dir/rust" && cargo build --release --locked)

install -m 0755 "$project_dir/rust/target/release/downloader-desktop" \
  "$appdir/usr/bin/downloader-desktop"
install -m 0755 "$project_dir/packaging/AppRun" "$appdir/AppRun"
install -m 0644 "$project_dir/packaging/io.github.xoykor.Downloader.desktop" \
  "$appdir/usr/share/applications/io.github.xoykor.Downloader.desktop"
install -m 0644 "$project_dir/packaging/io.github.xoykor.Downloader.svg" \
  "$appdir/usr/share/icons/hicolor/scalable/apps/io.github.xoykor.Downloader.svg"

copy_helper() {
  local name="$1" configured="${2:-}"
  local source="$configured"
  if [[ -z "$source" ]]; then
    source="$(command -v "$name" || true)"
  fi
  if [[ -z "$source" || ! -f "$source" ]]; then
    echo "Dependência ausente: $name (use ${name^^}_BINARY para informar o caminho)" >&2
    exit 1
  fi
  install -m 0755 "$source" "$appdir/usr/bin/$name"
}

copy_helper yt-dlp "${YTDLP_BINARY:-}"
copy_helper ffmpeg "${FFMPEG_BINARY:-}"
copy_helper ffprobe "${FFPROBE_BINARY:-}"

linuxdeploy="${LINUXDEPLOY:-$(command -v linuxdeploy || true)}"
if [[ -z "$linuxdeploy" ]]; then
  echo "linuxdeploy não encontrado; defina LINUXDEPLOY ou instale a ferramenta." >&2
  exit 1
fi

appimagetool="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
if [[ -z "$appimagetool" ]]; then
  echo "appimagetool não encontrado; defina APPIMAGETOOL ou instale a ferramenta." >&2
  exit 1
fi

chmod +x "$linuxdeploy" "$appimagetool"
export APPIMAGETOOL="$appimagetool"
export APPIMAGE_EXTRACT_AND_RUN=1
(
  cd "$output_dir"
  "$linuxdeploy" \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/downloader-desktop" \
    --executable "$appdir/usr/bin/ffmpeg" \
    --executable "$appdir/usr/bin/ffprobe" \
    --desktop-file "$appdir/usr/share/applications/io.github.xoykor.Downloader.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/io.github.xoykor.Downloader.svg" \
    --output appimage
)

image="$(find "$output_dir" -maxdepth 1 -type f -name '*.AppImage' -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
if [[ -z "$image" ]]; then
  echo "linuxdeploy não produziu um AppImage." >&2
  exit 1
fi
final="$output_dir/Downloader-x86_64.AppImage"
if [[ "$image" != "$final" ]]; then
  mv -f "$image" "$final"
fi
sha256sum "$final" > "$final.sha256"
echo "AppImage criado: $final"
