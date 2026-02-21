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
    "actions/symbolic/go-up-symbolic.svg"
    "actions/symbolic/go-previous-symbolic.svg"
    "actions/symbolic/go-next-symbolic.svg"
    "actions/symbolic/go-home-symbolic.svg"
    "actions/symbolic/document-save-symbolic.svg"
    "apps/symbolic/utilities-terminal-symbolic.svg"
    "apps/symbolic/system-file-manager-symbolic.svg"
    "apps/symbolic/preferences-desktop-apps-symbolic.svg"
    "apps/symbolic/accessories-calculator-symbolic.svg"
    "apps/symbolic/accessories-text-editor-symbolic.svg"
    "places/symbolic/folder-symbolic.svg"
    "places/symbolic/folder-documents-symbolic.svg"
    "places/symbolic/user-home-symbolic.svg"
    "mimetypes/symbolic/text-x-generic-symbolic.svg"
    "mimetypes/symbolic/application-x-executable-symbolic.svg"
    "devices/symbolic/computer-symbolic.svg"
    "devices/symbolic/network-wired-symbolic.svg"
    "apps/symbolic/web-browser-symbolic.svg"
    # Scalable (colorful) icons for app menu
    "apps/scalable/utilities-terminal.svg"
    "apps/scalable/system-file-manager.svg"
    "apps/scalable/preferences-desktop-apps.svg"
    "apps/scalable/accessories-calculator.svg"
    "apps/scalable/accessories-text-editor.svg"
    "apps/scalable/web-browser.svg"
    "places/scalable/folder.svg"
    "places/scalable/user-home.svg"
    "devices/scalable/computer.svg"
    "devices/scalable/network-wired.svg"
    "mimetypes/scalable/text-x-generic.svg"
    "mimetypes/scalable/application-x-executable.svg"
    "categories/scalable/help-about.svg"
    "categories/scalable/system-reboot.svg"
    "apps/scalable/doom.svg"
    "apps/scalable/system-monitor.svg"
    "apps/scalable/applications-science.svg"
    "apps/scalable/hardware.svg"
    "apps/scalable/unsettings.svg" # settings icon; toolbox in flat remix
    # Weather app icon (for desktop app menu launcher)
    "apps/scalable/weather-widget.svg"
    # Weather condition icons (panel, colorful, used by weather.elf)
    "panel/weather-clear.svg"
    "panel/weather-clear-night.svg"
    "panel/weather-few-clouds.svg"
    "panel/weather-few-clouds-night.svg"
    "panel/weather-clouds.svg"
    "panel/weather-clouds-night.svg"
    "panel/weather-overcast.svg"
    "panel/weather-mist.svg"
    "panel/weather-fog.svg"
    "panel/weather-showers.svg"
    "panel/weather-showers-scattered.svg"
    "panel/weather-snow.svg"
    "panel/weather-snow-scattered.svg"
    "panel/weather-snow-rain.svg"
    "panel/weather-freezing-rain.svg"
    "panel/weather-hail.svg"
    "panel/weather-storm.svg"
    "panel/weather-many-clouds.svg"
    "panel/weather-windy.svg"
    "panel/weather-severe-alert.svg"
    "panel/weather-none-available.svg"
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
