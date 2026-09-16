#!/usr/bin/env bash
set -euo pipefail

project_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
binary="$project_dir/rust/target/release/downloader-desktop"
user_home="${HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}"
bin_dir="$user_home/.local/bin"
applications_dir="$user_home/.local/share/applications"

if [[ ! -x "$binary" ]]; then
  echo "Compilando Downloader em modo release..."
  (cd "$project_dir/rust" && cargo build --release --locked)
fi

mkdir -p "$bin_dir" "$applications_dir"
install -m 0755 "$binary" "$bin_dir/downloader-desktop"
cat > "$applications_dir/downloader.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Downloader
Comment=Baixe e converta mídia com yt-dlp e FFmpeg
Exec=$bin_dir/downloader-desktop
TryExec=$bin_dir/downloader-desktop
Path=$project_dir/rust
Terminal=false
Categories=AudioVideo;
StartupNotify=true
EOF

echo "Atalho instalado em: $applications_dir/downloader.desktop"
echo "Abra o menu de aplicativos e procure por Downloader."
