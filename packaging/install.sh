#!/usr/bin/env bash
#
# Installs the sc0710 driver and quadcap so that both survive a reboot and a kernel upgrade.
#
# The driver goes in through DKMS rather than a hand-built module in a source tree, for two reasons
# that matter on someone else's machine: DKMS rebuilds it automatically when the kernel updates, and
# on Debian/Ubuntu it signs the result with the shim MOK, which is what lets it load under Secure
# Boot at all.
#
# Safe to re-run.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVER_SRC="${PROJECT_DIR}/third_party/sc0710"
PACKAGE_NAME="sc0710"
MODPROBE_CONF="/etc/modprobe.d/quadcap-sc0710.conf"
MODULES_LOAD_CONF="/etc/modules-load.d/quadcap-sc0710.conf"
UDEV_RULE="/etc/udev/rules.d/70-quadcap.rules"
EDID_CONF="/etc/quadcap/edid.conf"
EDID_UNIT="/etc/systemd/system/quadcap-edid.service"
# Tuned on an Elgato 4K60 Pro MK.2. hw_tonemap moves the HDR->SDR map onto the card's MCU (the host
# path costs about two thirds of the frame rate), and procedural_timings makes the driver take the
# capture geometry from its own registers instead of a timing table that misidentifies some UHD
# modes as DCI 4K.
MODULE_OPTIONS="hw_tonemap=1 procedural_timings=1"

# Reads /proc/modules rather than piping lsmod into grep: under `set -o pipefail` a `grep -q`
# short-circuits, lsmod dies of SIGPIPE, and the pipeline reports failure for a module that is
# actually loaded.
module_loaded() { grep -qs "^${PACKAGE_NAME} " /proc/modules; }

info()  { printf '\033[1;34m::\033[0m %s\n' "$*"; }
ok()    { printf '\033[1;32m ok\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m  !\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31merr\033[0m %s\n' "$*" >&2; exit 1; }

[[ ${EUID} -eq 0 ]] || die "Run this with sudo: sudo ${BASH_SOURCE[0]}"

# The user who invoked sudo, so group changes land on a real account rather than root.
TARGET_USER="${SUDO_USER:-}"

require_packages() {
  info "Checking build prerequisites"
  local missing=()
  for tool in dkms make gcc; do
    command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
  done
  [[ -d "/lib/modules/$(uname -r)/build" ]] || missing+=("linux-headers-$(uname -r)")

  if ((${#missing[@]})); then
    warn "Missing: ${missing[*]}"
    if command -v apt-get >/dev/null 2>&1; then
      info "Installing them with apt"
      apt-get update -qq
      apt-get install -y dkms build-essential "linux-headers-$(uname -r)"
    else
      die "Install these first: ${missing[*]}"
    fi
  fi
  ok "Build prerequisites present"
}

driver_version() {
  # dkms.conf is the single source of truth for the version; don't duplicate it here.
  sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p;/^PACKAGE_VERSION=/q' "${DRIVER_SRC}/dkms.conf"
}

# The driver lives in third_party/. It is a separate upstream project rather than a submodule, so a
# copy of this repo may arrive without it; fetch the pinned commit rather than failing.
fetch_driver_if_missing() {
  [[ -f "${DRIVER_SRC}/dkms.conf" ]] && return 0

  local pin_file="${PROJECT_DIR}/third_party/sc0710.pin"
  [[ -f "${pin_file}" ]] || die "Driver source missing at ${DRIVER_SRC} and no pin file to fetch it"
  command -v git >/dev/null 2>&1 || die "git is needed to fetch the driver source"

  local pin
  pin="$(tr -d '[:space:]' <"${pin_file}")"
  info "Driver source missing; cloning sc0710 at pinned commit ${pin:0:12}"
  rm -rf "${DRIVER_SRC}"
  git clone --quiet https://github.com/Nakildias/sc0710.git "${DRIVER_SRC}"
  git -C "${DRIVER_SRC}" checkout --quiet "${pin}" ||
    die "Pinned commit ${pin} not found upstream"
  ok "Fetched driver source"
}

install_driver() {
  fetch_driver_if_missing
  [[ -f "${DRIVER_SRC}/dkms.conf" ]] || die "Driver source missing at ${DRIVER_SRC}"
  local version src_dir
  version="$(driver_version)"
  [[ -n "${version}" ]] || die "Could not read PACKAGE_VERSION from ${DRIVER_SRC}/dkms.conf"
  src_dir="/usr/src/${PACKAGE_NAME}-${version}"

  local dkms_state
  dkms_state="$(dkms status -m "${PACKAGE_NAME}" -v "${version}" 2>/dev/null || true)"
  if [[ "${dkms_state}" == *installed* ]]; then
    ok "sc0710 ${version} already installed via DKMS"
    return
  fi

  info "Staging driver source into ${src_dir}"
  rm -rf "${src_dir}"
  mkdir -p "${src_dir}"
  # Everything DKMS needs to rebuild on a future kernel, and nothing from a previous local build.
  # `version` is not optional: the Makefile generates lib/sc0710-version.h from it, and without it
  # every future rebuild fails with no obvious cause.
  local required=(lib Makefile dkms.conf version)
  for item in "${required[@]}"; do
    [[ -e "${DRIVER_SRC}/${item}" ]] || die "Driver source is incomplete: ${item} is missing"
    cp -a "${DRIVER_SRC}/${item}" "${src_dir}/"
  done
  [[ -d "${DRIVER_SRC}/scripts" ]] && cp -a "${DRIVER_SRC}/scripts" "${src_dir}/"
  # A stale generated header from a local build would shadow the one DKMS should regenerate.
  rm -f "${src_dir}/lib/sc0710-version.h"

  # dkms.conf calls this helper by absolute path, so it has to exist outside the source tree too.
  if [[ -f "${DRIVER_SRC}/scripts/sc0710-dkms-make.sh" ]]; then
    install -d /usr/lib/sc0710
    install -m 0755 "${DRIVER_SRC}/scripts/sc0710-dkms-make.sh" /usr/lib/sc0710/
    [[ -f "${DRIVER_SRC}/scripts/sc0710-dkms-lib.sh" ]] &&
      install -m 0755 "${DRIVER_SRC}/scripts/sc0710-dkms-lib.sh" /usr/lib/sc0710/
  fi

  info "Registering and building with DKMS (this compiles a kernel module, give it a minute)"
  dkms add -m "${PACKAGE_NAME}" -v "${version}" >/dev/null 2>&1 || true
  dkms build -m "${PACKAGE_NAME}" -v "${version}"
  dkms install -m "${PACKAGE_NAME}" -v "${version}" --force
  ok "Driver installed for kernel $(uname -r), and will rebuild on kernel updates"
}

configure_module() {
  info "Writing module options and boot-time loading"
  cat >"${MODPROBE_CONF}" <<EOF
# Installed by quadcap. See the Troubleshooting section of the README for why these are set.
options ${PACKAGE_NAME} ${MODULE_OPTIONS}
EOF
  echo "${PACKAGE_NAME}" >"${MODULES_LOAD_CONF}"
  ok "Options: ${MODULE_OPTIONS}"
  ok "Loads at boot via ${MODULES_LOAD_CONF}"

  # udev gives the video group access; without it the node is root-only on some systems.
  cat >"${UDEV_RULE}" <<'EOF'
# quadcap: let the video group use the capture card's V4L2 node.
SUBSYSTEM=="video4linux", ATTRS{vendor}=="0x12ab", ATTRS{device}=="0x0710", MODE="0660", GROUP="video"
EOF
  udevadm control --reload-rules >/dev/null 2>&1 || true
  udevadm trigger --subsystem-match=video4linux >/dev/null 2>&1 || true
}

install_edid_service() {
  info "Setting up boot-time EDID application"
  install -d /etc/quadcap

  # An older version of this installer wrote a different key (QUADCAP_EDID_SOURCE) for a service
  # that applied the edid_source control. That control turned out to do nothing, so the key is
  # obsolete — but simply "keeping the existing config" left the new service with nothing to do and
  # no visible error. Treat a config without the current key as stale and replace it.
  if [[ -f "${EDID_CONF}" ]] && ! grep -q '^[[:space:]]*QUADCAP_EDID_IMAGE=' "${EDID_CONF}"; then
    mv "${EDID_CONF}" "${EDID_CONF}.obsolete"
    warn "Replaced an outdated ${EDID_CONF} (kept as ${EDID_CONF}.obsolete)"
  fi

  if [[ ! -f "${EDID_CONF}" ]]; then
    cat >"${EDID_CONF}" <<'EOF'
# EDID image the card presents to the console. The console picks its output mode from this, so it
# decides whether your passthrough display can sync.
#
# Leave empty to keep the card's factory EDID, which advertises 4K60 with HDR. If your passthrough
# display cannot sync to that it will show "unsupported input", and you want a narrower image:
#
#   QUADCAP_EDID_IMAGE=/usr/local/share/quadcap/edid/1080p60-sdr.edid
#
# On the 4K60 Pro MK.2 this is volatile and is reapplied at every boot by quadcap-edid.service.
QUADCAP_EDID_IMAGE=
EOF
    ok "Wrote ${EDID_CONF} (factory EDID kept)"
  else
    local configured
    configured="$(sed -n 's/^[[:space:]]*QUADCAP_EDID_IMAGE=//p' "${EDID_CONF}" | tail -1)"
    ok "Kept ${EDID_CONF} (image: ${configured:-none, factory EDID})"
  fi

  install -m 0644 "${PROJECT_DIR}/packaging/quadcap-edid.service" "${EDID_UNIT}"
  systemctl daemon-reload
  systemctl enable quadcap-edid.service >/dev/null 2>&1 || warn "Could not enable quadcap-edid.service"
  systemctl restart quadcap-edid.service >/dev/null 2>&1 || true

  # Report what actually happened rather than a blanket success: a configured image that silently
  # failed to apply is otherwise invisible.
  local wanted
  wanted="$(sed -n 's/^[[:space:]]*QUADCAP_EDID_IMAGE=//p' "${EDID_CONF}" | tail -1)"
  if [[ -z "${wanted}" ]]; then
    ok "No EDID image configured; the card keeps its factory EDID"
  elif [[ ! -f "${wanted}" ]]; then
    warn "Configured EDID image does not exist: ${wanted}"
  else
    ok "EDID ${wanted##*/} applied, and reapplied at every boot"
  fi
}

secure_boot_enabled() {
  local candidates=(/sys/firmware/efi/efivars/SecureBoot-*)
  [[ -e "${candidates[0]}" ]] || return 1
  # 4-byte EFI attribute header, then the value, so the last byte is the flag.
  local value
  value="$(od -An -tu1 -j4 -N1 "${candidates[0]}" 2>/dev/null | tr -d '[:space:]')"
  [[ "${value}" == "1" ]]
}

handle_secure_boot() {
  if ! secure_boot_enabled; then
    ok "Secure Boot is off; no signing needed"
    return
  fi

  info "Secure Boot is enabled"
  # DKMS on Debian/Ubuntu signs with the shim MOK. If that key is already enrolled — which it is
  # whenever another DKMS module such as NVIDIA loads — there is nothing to do.
  if modprobe "${PACKAGE_NAME}" 2>/dev/null && module_loaded; then
    ok "Module loaded, so its signing key is already trusted"
    return
  fi

  warn "The module did not load. Under Secure Boot this is almost always an unenrolled signing key."
  if command -v update-secureboot-policy >/dev/null 2>&1; then
    info "Enrolling the DKMS signing key — you will be asked to choose a password"
    update-secureboot-policy --enroll-key || true
    cat <<'EOF'

  ACTION NEEDED
    1. Reboot.
    2. At the blue "MOK Manager" screen choose "Enroll MOK", then "Continue".
    3. Enter the password you just set.
    4. The machine boots and the driver loads by itself from then on.

EOF
  else
    warn "update-secureboot-policy is not available. Either enroll"
    warn "/var/lib/shim-signed/mok/MOK.der with mokutil --import, or disable Secure Boot."
  fi
}

# Everything OBS needs to read quadcap: a loopback camera for the picture and one sink per audio
# source. Optional — a machine that will never stream should not be made to carry a kernel module —
# so a failure here warns and moves on rather than stopping the install.
install_obs_bridge() {
  if ! command -v apt-get >/dev/null 2>&1; then
    warn "Not a Debian-based system; skipping the OBS bridge"
    return 0
  fi

  info "Installing the OBS bridge"
  if ! apt-get install -y v4l2loopback-dkms >/dev/null 2>&1; then
    warn "Could not install v4l2loopback-dkms; OBS video output will be unavailable"
  else
    install -Dm0644 "${PROJECT_DIR}/packaging/quadcap-v4l2loopback.conf" \
      /etc/modprobe.d/quadcap-v4l2loopback.conf
    echo v4l2loopback >/etc/modules-load.d/quadcap-v4l2loopback.conf
    # Reloaded rather than just loaded, so a module already up with different options picks up
    # the card_label quadcap looks for instead of staying on whatever it was given before.
    modprobe -r v4l2loopback 2>/dev/null || true
    if modprobe v4l2loopback 2>/dev/null; then
      ok "Virtual camera ready; OBS will list it as \"quadcap\""
    else
      warn "v4l2loopback did not load; it should come up on the next boot"
    fi
  fi

  # PipeWire reads drop-ins from here at session start, so the two sinks exist before quadcap runs
  # and persist across reboots.
  if [[ -d /etc/pipewire ]]; then
    install -Dm0644 "${PROJECT_DIR}/packaging/pipewire/quadcap-obs-sinks.conf" \
      /etc/pipewire/pipewire.conf.d/quadcap-obs-sinks.conf
    ok "OBS audio sinks installed; log out and back in for them to appear"
  else
    warn "PipeWire not found; OBS audio output will be unavailable"
  fi
}

add_to_video_group() {
  [[ -n "${TARGET_USER}" ]] || return 0
  if [[ " $(id -nG "${TARGET_USER}") " == *" video "* ]]; then
    ok "${TARGET_USER} is already in the video group"
    return
  fi
  usermod -aG video "${TARGET_USER}"
  warn "Added ${TARGET_USER} to the video group — log out and back in for it to take effect"
}

install_app() {
  local prebuilt_dir="${PROJECT_DIR}/prebuilt/linux-x86_64"
  local build_dir="${PROJECT_DIR}/build"
  if [[ -x "${prebuilt_dir}/quadcap" && -x "${prebuilt_dir}/quadcapd" ]]; then
    if command -v apt-get >/dev/null 2>&1; then
      info "Installing application runtime dependencies"
      apt-get update -qq
      apt-get install -y \
        qt6-base-dev qt6-declarative-dev \
        qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript \
        qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
        qml6-module-qtquick-templates qml6-module-qtquick-window \
        gstreamer1.0-alsa gstreamer1.0-gl gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
        gstreamer1.0-pipewire gstreamer1.0-qt6
    fi
    # The bundled binaries are built on Ubuntu 24.04. On a distribution with a
    # different glibc or Qt 6 minor they will not load, and installing them
    # regardless would leave behind a command that cannot start. Asking one of
    # them to print its usage exercises the whole link before anything is
    # copied, so the source build below catches the cases the bundle misses.
    if QT_QPA_PLATFORM=offscreen "${prebuilt_dir}/quadcapd" --help >/dev/null 2>&1; then
      info "Installing the prebuilt application"
      install -Dm0755 "${prebuilt_dir}/quadcap" /usr/local/bin/quadcap
      install -Dm0755 "${prebuilt_dir}/quadcapd" /usr/local/bin/quadcapd
      install -Dm0644 "${PROJECT_DIR}/packaging/quadcap.desktop" \
        /usr/local/share/applications/quadcap.desktop
      install -Dm0644 "${PROJECT_DIR}/packaging/quadcap.svg" \
        /usr/local/share/icons/hicolor/scalable/apps/quadcap.svg
      install -Dm0755 "${PROJECT_DIR}/packaging/uninstall.sh" \
        /usr/local/share/quadcap/uninstall.sh
      install -Dm0644 "${PROJECT_DIR}/INSTALL.md" /usr/local/share/quadcap/INSTALL.md
      install -d /usr/local/share/quadcap/edid
      install -m0644 "${PROJECT_DIR}"/packaging/edid/* /usr/local/share/quadcap/edid/
      ok "Installed quadcap and quadcapd into /usr/local/bin"
      return
    fi
    warn "The bundled binaries do not run here; falling back to a build from source"
  fi
  if [[ ! -x "${build_dir}/quadcap" ]]; then
    warn "No built application at ${build_dir}/quadcap; skipping app install"
    warn "Build it with: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
    return
  fi
  info "Installing the application"
  cmake --install "${build_dir}" --prefix /usr/local >/dev/null
  ok "Installed quadcap and quadcapd into /usr/local/bin"
}

main() {
  info "Installing quadcap from ${PROJECT_DIR}"
  require_packages
  install_driver
  configure_module
  handle_secure_boot
  install_edid_service
  install_obs_bridge
  add_to_video_group
  install_app

  echo
  if module_loaded; then
    ok "Driver is loaded. Start the app with: quadcap"
  else
    warn "Driver is not loaded yet — see the action above, then reboot."
  fi
}

main "$@"
