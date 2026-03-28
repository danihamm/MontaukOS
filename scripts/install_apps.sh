#!/bin/bash
# install_apps.sh - Bundle standalone apps into bin/apps/<name>/ directories
# Each bundle contains: binary, manifest.toml, icon SVG
# Usage: ./scripts/install_apps.sh (from project root)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SRC="$PROJECT_ROOT/programs/src"
BIN="$PROJECT_ROOT/programs/bin"
ICON_SRC="$PROJECT_ROOT/programs/gui/icons/Flat-Remix-Blue-Light-darkPanel"

# App definitions: name|icon_source_path
# The binary is already built by each app's Makefile into bin/apps/<name>/
APPS=(
    "doom|apps/scalable/doom.svg"
    "spreadsheet|mimetypes/scalable/spreadsheet.svg"
    "wordprocessor|apps/scalable/libreoffice6.0-writer.svg"
    "weather|apps/scalable/weather-widget.svg"
    "wikipedia|apps/scalable/web-browser.svg"
    "imageviewer|apps/scalable/utilities-terminal.svg"
    "fontpreview|apps/scalable/utilities-terminal.svg"
    "pdfviewer|apps/scalable/utilities-terminal.svg"
    "disks|apps/scalable/gparted.svg"
    "devexplorer|apps/scalable/hardware.svg"
    "installer|mimetypes/scalable/text-x-install.svg"
    "volume|apps/scalable/pavucontrol.svg"
    "music|apps/scalable/audio-player.svg"
    "video|apps/scalable/video-player.svg"
    "bluetooth|apps/scalable/bluetooth.svg"
    "terminal|apps/scalable/utilities-terminal.svg"
    "klog|apps/scalable/utilities-terminal.svg"
    "procmgr|apps/scalable/system-monitor.svg"
    "calculator|apps/scalable/accessories-calculator.svg"
    "rpgdemo|apps/scalable/utilities-terminal.svg"
    "paint|apps/scalable/kolourpaint.svg"
    "screenshot|apps/scalable/gnome-screenshot.svg"
    "texteditor|apps/scalable/accessories-text-editor.svg"
    "mandelbrot|apps/scalable/applications-science.svg"
)

installed=0
for entry in "${APPS[@]}"; do
    IFS='|' read -r name icon_rel <<< "$entry"

    app_dir="$BIN/apps/$name"
    mkdir -p "$app_dir"

    # Copy manifest
    manifest="$SRC/$name/manifest.toml"
    if [ -f "$manifest" ]; then
        cp "$manifest" "$app_dir/manifest.toml"
    else
        echo "install_apps: warning: $manifest not found" >&2
    fi

    # Copy icon. Prefer a local app icon file when one exists; otherwise fall back
    # to the shared theme asset declared in the app table above.
    icon_src="$ICON_SRC/$icon_rel"
    icon_name=$(grep '^icon' "$manifest" 2>/dev/null | sed 's/.*= *"\(.*\)"/\1/' || echo "")
    local_icon="$SRC/$name/$icon_name"
    if [ -n "$icon_name" ] && [ -f "$local_icon" ]; then
        cp "$local_icon" "$app_dir/$icon_name"
    elif [ -n "$icon_name" ] && [ -f "$icon_src" ]; then
        cp "$icon_src" "$app_dir/$icon_name"
    elif [ -n "$icon_name" ]; then
        echo "install_apps: warning: icon $local_icon or $icon_src not found for $name" >&2
    fi

    installed=$((installed + 1))
done

# Special case: copy doom1.wad alongside doom binary
if [ -f "$PROJECT_ROOT/programs/data/games/doom1.wad" ]; then
    cp "$PROJECT_ROOT/programs/data/games/doom1.wad" "$BIN/apps/doom/doom1.wad"
fi

echo "install_apps: installed $installed app bundles to $BIN/apps/"
