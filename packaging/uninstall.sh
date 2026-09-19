#!/usr/bin/env bash
#
# Removes everything packaging/install.sh put on the system.
#
# Leaves recordings and the flashback ring alone — those are the user's, not ours.

set -euo pipefail

PACKAGE_NAME="sc0710"
MODPROBE_CONF="/etc/modprobe.d/quadcap-sc0710.conf"
MODULES_LOAD_CONF="/etc/modules-load.d/quadcap-sc0710.conf"
UDEV_RULE="/etc/udev/rules.d/70-quadcap.rules"

info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m ok\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m  !\033[0m %s\n' "$*"; }

[[ ${EUID} -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }

info "Unloading the module"
# PipeWire holds V4L2 nodes open even with no app running, so a plain rmmod can legitimately fail.
if lsmod | grep -q "^${PACKAGE_NAME}"; then
  modprobe -r "${PACKAGE_NAME}" 2>/dev/null ||
    warn "Module busy; it will be gone after a reboot"
fi

info "Removing DKMS registrations"
while read -r version; do
  [[ -n "${version}" ]] || continue
  dkms remove -m "${PACKAGE_NAME}" -v "${version}" --all 2>/dev/null || true
  rm -rf "/usr/src/${PACKAGE_NAME}-${version}"
  ok "Removed ${PACKAGE_NAME} ${version}"
done < <(dkms status -m "${PACKAGE_NAME}" 2>/dev/null | sed -n 's/^[^/]*\/\([^,:]*\).*/\1/p' | sort -u)

# Clean up the unit and its config wherever it came from, including installs that predate the
# current layout.
if [[ -f /etc/systemd/system/quadcap-edid.service ]]; then
  info "Removing the obsolete boot-time EDID service"
  systemctl disable --now quadcap-edid.service >/dev/null 2>&1 || true
  rm -f /etc/systemd/system/quadcap-edid.service
  rm -rf /etc/quadcap
  systemctl daemon-reload >/dev/null 2>&1 || true
fi

info "Removing configuration"
rm -f "${MODPROBE_CONF}" "${MODULES_LOAD_CONF}" "${UDEV_RULE}"
rm -rf /usr/lib/sc0710
udevadm control --reload-rules >/dev/null 2>&1 || true

info "Removing the application"
rm -f /usr/local/bin/quadcap /usr/local/bin/quadcapd
rm -f /usr/local/share/applications/quadcap.desktop
rm -f /usr/local/share/icons/hicolor/scalable/apps/quadcap.svg

echo
ok "Uninstalled. Recordings and the flashback ring were left untouched."
warn "The Secure Boot signing key was left enrolled; other DKMS modules may rely on it."
