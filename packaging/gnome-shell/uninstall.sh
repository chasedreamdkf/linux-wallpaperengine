#!/usr/bin/env bash
#
# Removes the linux-wallpaperengine GNOME Shell backend installed by install.sh
# or run.sh.

set -euo pipefail

UUID="linux-wallpaperengine-gnome@local"
EXT_DEST="${HOME}/.local/share/gnome-shell/extensions/${UUID}"
SYSTEMD_DIR="${HOME}/.config/systemd/user"
UNIT_NAME="linux-wallpaperengine-gnome.service"
CONF_DIR="${HOME}/.config/linux-wallpaperengine-gnome"

systemctl --user disable --now "${UNIT_NAME}" 2>/dev/null || true

gnome-extensions disable "${UUID}" 2>/dev/null || true

if [[ -f "${SYSTEMD_DIR}/${UNIT_NAME}" ]]; then
    rm -f "${SYSTEMD_DIR}/${UNIT_NAME}"
    systemctl --user daemon-reload || true
fi

if [[ -d "${EXT_DEST}" ]]; then
    rm -rf "${EXT_DEST}"
fi

if [[ -d "${CONF_DIR}" ]]; then
    rm -rf "${CONF_DIR}"
fi

# clean up any shared memory segments left behind by a hard crash
rm -f /dev/shm/lwe-gnome-* 2>/dev/null || true

echo "Uninstalled ${UUID}. Log out and back in to fully unload the extension."