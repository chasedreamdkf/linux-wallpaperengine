#!/usr/bin/env bash
#
# Installs the linux-wallpaperengine GNOME Shell (Hidamari-style) backend:
#   - the GNOME Shell extension into the user extensions directory
#   - a systemd --user unit for the engine
#
# Usage:
#   ./install.sh [engine-binary] [wallpaper-path]
#
# If no engine binary is given the script looks for `linux-wallpaperengine` in
# PATH and falls back to the repository build output. If no wallpaper path is
# given the unit is installed with a placeholder you have to edit afterwards.

set -euo pipefail

UUID="linux-wallpaperengine-gnome@local"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT_SRC="$(cd "${SCRIPT_DIR}/../../extensions/${UUID}" && pwd)"

EXT_DEST="${HOME}/.local/share/gnome-shell/extensions/${UUID}"
SYSTEMD_DIR="${HOME}/.config/systemd/user"
UNIT_NAME="linux-wallpaperengine-gnome.service"

BINARY="${1:-}"
BACKGROUND="${2:-}"

if [[ -z "${BINARY}" ]]; then
    if command -v linux-wallpaperengine >/dev/null 2>&1; then
        BINARY="$(command -v linux-wallpaperengine)"
    elif [[ -x "${SCRIPT_DIR}/../../build/output/linux-wallpaperengine" ]]; then
        BINARY="$(cd "${SCRIPT_DIR}/../.." && pwd)/build/output/linux-wallpaperengine"
    fi
fi

if [[ -z "${BINARY}" ]]; then
    echo "Could not locate the linux-wallpaperengine binary; pass it as the first argument." >&2
    exit 1
fi

echo "Installing extension into ${EXT_DEST}"
mkdir -p "${EXT_DEST}"
cp -f \
    "${EXT_SRC}/metadata.json" \
    "${EXT_SRC}/extension.js" \
    "${EXT_SRC}/dbus.js" \
    "${EXT_SRC}/shm.js" \
    "${EXT_SRC}/state.js" \
    "${EXT_DEST}/"

echo "Installing systemd user unit into ${SYSTEMD_DIR}"
mkdir -p "${SYSTEMD_DIR}"

if [[ -z "${BACKGROUND}" ]]; then
    BACKGROUND="/path/to/wallpaper"
    echo "WARNING: no wallpaper path given, edit ${SYSTEMD_DIR}/${UNIT_NAME} before starting the service" >&2
fi

sed -e "s|@BINARY@|${BINARY}|g" -e "s|@BACKGROUND@|${BACKGROUND}|g" \
    "${SCRIPT_DIR}/${UNIT_NAME}" > "${SYSTEMD_DIR}/${UNIT_NAME}"

systemctl --user daemon-reload || true

cat <<EOF

Installation complete.

Enable everything with:
  gnome-extensions enable ${UUID}
  systemctl --user enable --now ${UNIT_NAME}

On Wayland you must log out and back in (or restart GNOME Shell) before the
extension is loaded.
EOF