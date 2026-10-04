#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
bin_home=${XDG_BIN_HOME:-"$HOME/.local/bin"}
lib_dir="$HOME/.local/lib/glance"
desktop_dir="$data_home/applications"
icon_dir="$data_home/icons/hicolor/scalable/apps"

"$repo_dir/scripts/build.sh"

mkdir -p "$lib_dir" "$bin_home" "$desktop_dir" "$icon_dir"
install -m 755 "$repo_dir/build/glance" "$lib_dir/glance"
rm -rf "$lib_dir/qml"
cp -R "$repo_dir/qml" "$lib_dir/qml"
find "$lib_dir/qml" -type f -exec chmod 644 {} +
find "$lib_dir/qml" -type d -exec chmod 755 {} +
ln -sfn "$lib_dir/glance" "$bin_home/glance"
install -m 644 "$repo_dir/packaging/glance.desktop" "$desktop_dir/glance.desktop"
install -m 644 "$repo_dir/packaging/glance.svg" "$icon_dir/glance.svg"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$desktop_dir"
fi

if [ "${1:-}" = "--set-defaults" ]; then
    for mime in application/pdf image/png image/jpeg image/gif image/webp image/bmp image/tiff; do
        xdg-mime default glance.desktop "$mime"
    done
fi

printf 'Installed Glance to %s\n' "$lib_dir/glance"
case ":$PATH:" in
    *":$bin_home:"*) ;;
    *) printf 'Add %s to PATH to run glance from a shell.\n' "$bin_home" ;;
esac
