#!/usr/bin/env bash
#
# One-stop entry point for the GNOME Shell (Hidamari-style) backend: installs or
# refreshes the extension, writes and enables a systemd --user unit and starts
# the engine. Because the unit is enabled it also comes back on every login.
#
# Usage:
#   ./run.sh                                reuse the wallpaper(s) used last time
#   ./run.sh 2976403740                     one wallpaper across the whole desktop
#   ./run.sh 0=2976403740 1=3023217614      one wallpaper per monitor
#
# Set LWE_FRAME_SCALE (0.1 - 1.0) to render the frames at a fraction of the
# desktop resolution. GNOME Shell then has far less data to copy and upload
# every frame, which keeps the desktop responsive at the price of a softer
# image:
#
#   LWE_FRAME_SCALE=0.75 ./run.sh 2976403740
#
# For the per-monitor form <index> is the 0-based monitor index the extension
# reports; the engine logs them on startup:
#
#   GNOME Shell desktop bounding box is 3840x1285 at (0,0) across 2 monitor(s)
#     monitor 0 -> 1920x85:1920x1200
#     monitor 1 -> 0x0:1920x1080

set -euo pipefail

UUID="linux-wallpaperengine-gnome@local"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
EXT_SRC="${REPO_DIR}/extensions/${UUID}"
EXT_DEST="${HOME}/.local/share/gnome-shell/extensions/${UUID}"
SYSTEMD_DIR="${HOME}/.config/systemd/user"
UNIT_NAME="linux-wallpaperengine-gnome.service"
ARGS_FILE="${HOME}/.config/linux-wallpaperengine-gnome/args"
BUS_NAME="org.gnome.Shell.Extensions.LinuxWallpaperEngine"

die () {
    echo "error: $*" >&2
    exit 1
}

# --- locate the engine binary -------------------------------------------------
BINARY=""

if command -v linux-wallpaperengine >/dev/null 2>&1; then
    BINARY="$(command -v linux-wallpaperengine)"
elif [[ -x "${REPO_DIR}/build/output/linux-wallpaperengine" ]]; then
    BINARY="${REPO_DIR}/build/output/linux-wallpaperengine"
fi

[[ -n "${BINARY}" ]] || die "cannot find the linux-wallpaperengine binary, build it first"

# --- work out the wallpaper arguments -----------------------------------------
ARGS=("$@")

if [[ ${#ARGS[@]} -eq 0 ]]; then
    [[ -f "${ARGS_FILE}" ]] || die "no wallpaper given and none saved yet, see the usage at the top of this script"

    mapfile -t ARGS < "${ARGS_FILE}"
    echo "Reusing the wallpaper(s) from ${ARGS_FILE}: ${ARGS[*]}"
fi

# the plain form spans the whole desktop, the <index>=<background> form is per monitor
HAS_INDEX=0
HAS_PLAIN=0

for arg in "${ARGS[@]}"; do
    if [[ "${arg}" == *=* ]]; then
        HAS_INDEX=1
    else
        HAS_PLAIN=1
    fi
done

if [[ ${HAS_INDEX} -eq 1 && ${HAS_PLAIN} -eq 1 ]]; then
    die "cannot mix <background> with <index>=<background>"
fi

ENGINE_ARGS=()

if [[ ${HAS_PLAIN} -eq 1 ]]; then
    [[ ${#ARGS[@]} -eq 1 ]] || die "only one <background> can be given without a monitor index"

    ENGINE_ARGS=(--gnome-shell "${ARGS[0]}")
else
    for arg in "${ARGS[@]}"; do
        index="${arg%%=*}"
        background="${arg#*=}"

        [[ "${index}" =~ ^[0-9]+$ ]] || die "'${arg}' does not start with a monitor index (0, 1, ...)"
        [[ -n "${background}" ]] || die "'${arg}' does not name a background"

        ENGINE_ARGS+=(--gnome-shell-screen "${index}" --bg "${background}" --scaling fill)
    done
fi

# optional: render the frames at a fraction of the desktop resolution to lower the
# per-frame copy/upload cost inside GNOME Shell (at the price of a softer image)
if [[ -n "${LWE_FRAME_SCALE:-}" ]]; then
    ENGINE_ARGS+=(--frame-scale "${LWE_FRAME_SCALE}")
fi

# --- install or refresh the extension -----------------------------------------
echo "Installing the extension into ${EXT_DEST}"
mkdir -p "${EXT_DEST}"
cp -f \
    "${EXT_SRC}/metadata.json" \
    "${EXT_SRC}/extension.js" \
    "${EXT_SRC}/dbus.js" \
    "${EXT_SRC}/shm.js" \
    "${EXT_SRC}/state.js" \
    "${EXT_DEST}/"

# --- remember the arguments so a later run without arguments reuses them ------
mkdir -p "$(dirname "${ARGS_FILE}")"
printf '%s\n' "${ARGS[@]}" > "${ARGS_FILE}"

# --- write, enable and (when possible) start the unit -------------------------
mkdir -p "${SYSTEMD_DIR}"
sed -e "s|@BINARY@|${BINARY}|g" -e "s|@ARGS@|${ENGINE_ARGS[*]}|g" \
    "${SCRIPT_DIR}/${UNIT_NAME}" > "${SYSTEMD_DIR}/${UNIT_NAME}"

systemctl --user daemon-reload
systemctl --user enable "${UNIT_NAME}"

# GNOME Shell only imports an extension once, so a freshly installed one is not
# loaded until the next login. Starting the engine before that would only make it
# exit, so wait for the extension's bus name instead.
if gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
	--method org.freedesktop.DBus.NameHasOwner "${BUS_NAME}" 2>/dev/null | grep -q '(true,)'; then
    systemctl --user restart "${UNIT_NAME}"
    echo "Engine started (${UNIT_NAME})"
else
    cat <<EOF

The GNOME Shell bridge extension is not loaded in this session yet, so the engine
was not started. Log out and back in once: the unit is enabled and will start
automatically, and the wallpaper now comes back on every login.
EOF
fi

echo
echo "Binary:    ${BINARY}"
echo "Arguments: ${ENGINE_ARGS[*]}"
echo "Logs:      journalctl --user -u ${UNIT_NAME} -f"
echo "Stop:      systemctl --user disable --now ${UNIT_NAME}"
