#!/usr/bin/env bash
set -euo pipefail

project_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build_root="${APPIMAGE_BUILD_DIR:-$project_dir/build/appimage}"
cmake_dir="$build_root/cmake"
appdir="$build_root/AppDir"
output_dir="${APPIMAGE_OUTPUT_DIR:-$project_dir/dist}"

rm -rf "$appdir" "$cmake_dir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" \
  "$appdir/usr/share/icons/hicolor/scalable/apps" "$output_dir"
rm -f "$output_dir"/*.AppImage "$output_dir"/*.AppImage.sha256

cmake -S "$project_dir/c" -B "$cmake_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDOWNLOADER_BUILD_DESKTOP=ON \
  -DDOWNLOADER_WARNINGS_AS_ERRORS=ON
cmake --build "$cmake_dir" --parallel
ctest --test-dir "$cmake_dir" --output-on-failure

install -m 0755 "$cmake_dir/downloader-desktop" "$appdir/usr/bin/downloader-desktop"
install -m 0755 "$cmake_dir/downloader-cli" "$appdir/usr/bin/downloader-cli"
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
    echo "Dependência ausente: $name" >&2
    exit 1
  fi
  install -m 0755 "$source" "$appdir/usr/bin/$name"
}

copy_helper yt-dlp "${YTDLP_BINARY:-}"
copy_helper ffmpeg "${FFMPEG_BINARY:-}"
copy_helper ffprobe "${FFPROBE_BINARY:-}"

# Usa o shim local (packaging/.bin/linuxdeploy), que injeta --strip no após
# --output. O linuxdeploy padrão do PATH é um AppImage com binutils 2.35 cujo
# strip não consegue ler seções .relr.dyn (SHT_RELR=0x13) emitidas por
# glibc 2.44/binutils 2.47, abortando a empacotagem.
linuxdeploy="${LINUXDEPLOY:-$project_dir/packaging/.bin/linuxdeploy}"
appimagetool="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
if [[ -z "$linuxdeploy" || -z "$appimagetool" ]]; then
  echo "linuxdeploy e appimagetool são necessários para criar o AppImage." >&2
  exit 1
fi
chmod +x "$linuxdeploy" "$appimagetool"
export APPIMAGETOOL="$appimagetool"
export APPIMAGE_EXTRACT_AND_RUN=1

# AppImageKit's bundled binutils 2.35 strip cannot parse SHT_RELR (.relr.dyn)
# sections emitted by the glibc 2.44/binutils 2.47 toolchain, aborting packaging
# on any collected library that carries them (GTK4/glib/cairo/pango here). No
# newer linuxdeploy is available and this build has no --strip flag to disable it.
# Modern /usr/bin/strip handles .relr.dyn fine, so we ship already-stripped copies
# of every shared library our executables depend on into the AppDir; linuxdeploy
# then finds them present and never re-copies the relr-bearing originals from the
# system. Host filesystem is left untouched.
prestrip_libs() {
  local exe src base dst
  for exe in "$cmake_dir/downloader-desktop" "$cmake_dir/downloader-cli"; do
    [[ -f "$exe" ]] || continue
    while IFS= read -r src; do
      [[ -z "$src" ]] && continue
      base="$(basename -- "$src")"
      dst="$appdir/usr/lib/$base"
      if [[ ! -e "$dst" ]]; then
        cp -P "$src" "$dst" 2>/dev/null || continue
        /usr/bin/strip "$dst" 2>/dev/null || true
      fi
    done < <(ldd -- "$exe" 2>/dev/null | awk '/=>/{print $1}')
  done
}
prestrip_libs

(
  cd "$output_dir"
  "$linuxdeploy" \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/downloader-desktop" \
    --executable "$appdir/usr/bin/downloader-cli" \
    --executable "$appdir/usr/bin/ffmpeg" \
    --executable "$appdir/usr/bin/ffprobe" \
    --desktop-file "$appdir/usr/share/applications/io.github.xoykor.Downloader.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/io.github.xoykor.Downloader.svg" \
    --output appimage
)

image="$(find "$output_dir" -maxdepth 1 -type f -name '*.AppImage' -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
if [[ -z "$image" ]]; then
  echo "linuxdeploy não produziu AppImage." >&2
  exit 1
fi
final="$output_dir/Downloader-x86_64.AppImage"
if [[ "$image" != "$final" ]]; then mv -f "$image" "$final"; fi
sha256sum "$final" > "$final.sha256"
echo "AppImage criado: $final"
