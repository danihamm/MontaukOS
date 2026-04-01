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
    "actions/symbolic/edit-undo-symbolic.svg"
    "actions/symbolic/edit-redo-symbolic.svg"
    "actions/symbolic/format-justify-left-symbolic.svg"
    "actions/symbolic/format-justify-center-symbolic.svg"
    "actions/symbolic/format-justify-right-symbolic.svg"
    "actions/symbolic/view-list-bullet-symbolic.svg"
    "actions/symbolic/view-list-ordered-symbolic.svg"
    "actions/symbolic/format-indent-less-symbolic.svg"
    "actions/symbolic/format-indent-more-symbolic.svg"
    "apps/symbolic/utilities-terminal-symbolic.svg"
    "apps/symbolic/system-file-manager-symbolic.svg"
    "apps/symbolic/preferences-desktop-apps-symbolic.svg"
    "apps/symbolic/accessories-calculator-symbolic.svg"
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
    "apps/scalable/web-browser.svg"
    "places/scalable/folder.svg"
    "places/scalable/user-home.svg"
    "devices/scalable/computer.svg"
    "devices/scalable/network-wired.svg"
    "devices/scalable/printer.svg"
    "devices/symbolic/printer-symbolic.svg"
    "mimetypes/scalable/text-x-generic.svg"
    "mimetypes/scalable/application-x-executable.svg"
    "categories/scalable/help-about.svg"
    "categories/scalable/system-reboot.svg"
    "categories/scalable/system-shutdown.svg"
    "apps/scalable/system-monitor.svg"
    "apps/scalable/applications-science.svg"
    "apps/scalable/unsettings.svg" # settings icon; toolbox in flat remix
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
    "apps/symbolic/utilities-system-monitor-symbolic.svg" # system monitor
    "apps/scalable/utilities-system-monitor.svg" # system monitor
    "apps/scalable/drive-harddisk.svg"
    "actions/16/trash-empty.svg"
    "places/scalable/folder-blue-home.svg"
    "places/scalable/folder-blue-development.svg"
    "places/scalable/folder-blue-documents.svg"
    "places/scalable/folder-blue-desktop.svg"
    "places/scalable/folder-blue-music.svg"
    "places/scalable/folder-blue-videos.svg"
    "places/scalable/folder-blue-pictures.svg"
    "places/scalable/folder-blue-downloads.svg"
    "actions/16/edit-copy.svg"
    "actions/16/edit-cut.svg"
    "actions/16/edit-paste.svg"
    "actions/16/edit-rename.svg"
    "actions/16/folder-new.svg"
    "apps/scalable/pavucontrol.svg" # for volume control app
    "actions/16/media-pause.svg"
    "actions/16/media-play.svg"
    "actions/16/media-stop.svg"
    "actions/16/media-forward.svg"
    "actions/16/media-rewind.svg"
    "actions/16/view-media-equalizer.svg"
    "actions/16/media-playlist-shuffle.svg"
    "actions/16/media-playlist-repeat.svg"
    "actions/16/media-playlist-repeat-song.svg"
    "panel/media-playlist-shuffle-symbolic.svg"
    "panel/media-playlist-repeat-symbolic.svg"
    "panel/media-playlist-repeat-one-symbolic.svg"
    "apps/scalable/multimedia-video-player.svg"
    "apps/scalable/bluetooth.svg"
    "panel/volume-level-high.svg"
    "status/symbolic/audio-volume-high-symbolic.svg"
    "apps/scalable/gnome-logout.svg"
    "apps/scalable/lock.svg"
    "apps/scalable/sleep.svg"
    "apps/scalable/kolourpaint.svg" # paint app
    "panel/sensors-temperature-symbolic.svg" # temperature panel icon
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
