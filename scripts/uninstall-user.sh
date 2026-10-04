#!/bin/sh
set -eu

data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
bin_home=${XDG_BIN_HOME:-"$HOME/.local/bin"}

rm -f "$bin_home/glance"
rm -f "$HOME/.local/lib/glance/glance"
rm -rf "$HOME/.local/lib/glance/qml"
rmdir "$HOME/.local/lib/glance" 2>/dev/null || true
rm -f "$data_home/applications/glance.desktop"
rm -f "$data_home/icons/hicolor/scalable/apps/glance.svg"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$data_home/applications"
fi

printf 'Uninstalled Glance user files. MIME defaults were left unchanged.\n'
