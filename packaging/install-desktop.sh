#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build/c-release"
bin_dir="$HOME/.local/bin"
app_dir="$HOME/.local/share/applications"
icon_dir="$HOME/.local/share/icons/hicolor/scalable/apps"

cmake -S "$project_dir/c" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel

mkdir -p "$bin_dir" "$app_dir" "$icon_dir"
install -m 0755 "$build_dir/downloader-desktop" "$bin_dir/downloader-desktop"
install -m 0755 "$build_dir/downloader-cli" "$bin_dir/downloader-cli"
install -m 0644 "$project_dir/packaging/io.github.xoykor.Downloader.svg" \
  "$icon_dir/io.github.xoykor.Downloader.svg"

# O arquivo instalado recebe caminho absoluto para não depender do PATH da sessão gráfica.
sed "s|^Exec=.*|Exec=$bin_dir/downloader-desktop %U|" \
  "$project_dir/packaging/io.github.xoykor.Downloader.desktop" \
  > "$app_dir/io.github.xoykor.Downloader.desktop"
chmod 0644 "$app_dir/io.github.xoykor.Downloader.desktop"

echo "Downloader instalado. Procure por 'Downloader' no menu de aplicativos."
