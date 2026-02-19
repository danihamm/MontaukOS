#!/bin/bash
# copy_icons.sh - Copy selected SVG icons for the desktop environment
# Usage: ./scripts/copy_icons.sh (from project root or programs/ directory)

set -e

# Find project root relative to this script's location
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ICON_SRC="$PROJECT_ROOT/programs/gui/icons/Flat-Remix-Blue-Light-darkPanel"
ICON_DST="$PROJECT_ROOT/programs/bin/icons"

mkdir -p "$ICON_DST"

# Selected icons for the desktop
ICONS=(
    "actions/symbolic/view-app-grid-symbolic.svg"
    "actions/symbolic/window-close-symbolic.svg"
    "actions/symbolic/window-maximize-symbolic.svg"
    "actions/symbolic/window-minimize-symbolic.svg"
    "apps/symbolic/utilities-terminal-symbolic.svg"
    "apps/symbolic/system-file-manager-symbolic.svg"
    "apps/symbolic/preferences-desktop-apps-symbolic.svg"
    "places/symbolic/folder-symbolic.svg"
    "places/symbolic/folder-documents-symbolic.svg"
    "places/symbolic/user-home-symbolic.svg"
    "mimetypes/symbolic/text-x-generic-symbolic.svg"
    "mimetypes/symbolic/application-x-executable-symbolic.svg"
    "devices/symbolic/computer-symbolic.svg"
    "devices/symbolic/network-wired-symbolic.svg"
)

copied=0
for icon in "${ICONS[@]}"; do
    src="$ICON_SRC/$icon"
    # Extract just the filename
    name=$(basename "$icon")
    dst="$ICON_DST/$name"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        copied=$((copied + 1))
    else
        echo "copy_icons: warning: $src not found" >&2
    fi
done

echo "copy_icons: copied $copied icons to $ICON_DST"
