#!/usr/bin/env bash
#
# Builds a self-contained AppImage from an already-configured build tree.
#
# The AppImage exists for distributions where the .deb will not install: quadcap links Qt's QML
# private ABI, so a package built against one Qt patch release refuses to install against another.
# Bundling Qt and GStreamer sidesteps that entirely.
#
# Usage: make-appimage.sh BUILD_DIR OUTPUT_DIR

set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: $0 BUILD_DIR OUTPUT_DIR" >&2; exit 2; }

BUILD_DIR="$(realpath "$1")"
OUTPUT_DIR="$(mkdir -p "$2" && realpath "$2")"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n 's/^project(quadcap VERSION \([0-9.]*\).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")"
[[ -n "${VERSION}" ]] || { echo "could not read the project version" >&2; exit 1; }

WORK_DIR="$(mktemp -d)"
cleanup() { rm -rf "${WORK_DIR}"; }
trap cleanup EXIT

TOOL_DIR="${WORK_DIR}/tools"
mkdir -p "${TOOL_DIR}"

# Each tool is fetched by URL and then checked against the digest recorded beside it, so a rolling
# upstream tag cannot quietly change what runs here.
while read -r digest name url; do
  # A bare `[[ ... ]] && continue` returns 1 on every line that is not a comment, which set -e
  # reads as a failure.
  if [[ -z "${digest}" || "${digest}" == \#* ]]; then
    continue
  fi
  curl --fail --location --silent --show-error --output "${TOOL_DIR}/${name}" "${url}"
  echo "${digest}  ${TOOL_DIR}/${name}" | sha256sum --check --status \
    || { echo "${name} does not match packaging/appimage-tools.pin" >&2; exit 1; }
  chmod +x "${TOOL_DIR}/${name}"
done <"${PROJECT_DIR}/packaging/appimage-tools.pin"

APPDIR="${WORK_DIR}/AppDir"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}" --prefix /usr >/dev/null

# linuxdeploy-plugin-qt finds the QML imports an app needs by scanning its .qml sources; without
# this it ships the engine and none of the modules, and the window comes up empty.
export QML_SOURCES_PATHS="${PROJECT_DIR}/src/ui"
# qml6glsink lives in the Qt6 GStreamer plugin and is what puts the preview on screen, so the
# GStreamer plugin has to be told to take the whole set rather than the base ones.
export DEPLOY_GSTREAMER_INCLUDE_BAD_PLUGINS=1
# linuxdeploy-plugin-qt bundles only the xcb platform plugin by default. Current Ubuntu desktops
# default to a Wayland session, where that means running through XWayland at best and failing
# outright on a compositor without it, so the Wayland platform plugin and the client-side pieces it
# loads at runtime come along too. offscreen is what lets the build verify the binary starts.
# Qt splits the Wayland plugin differently between releases (libqwayland.so on recent ones,
# libqwayland-generic.so and libqwayland-egl.so on older), and naming one that is absent is a hard
# error, so take whichever this Qt actually ships.
# Asked of Qt itself rather than found on disk: this machine also carries a Qt5 tree and two
# NVIDIA tools with their own bundled plugins, and picking one of those names a plugin that the
# Qt6 deployment then cannot find.
QT_PLUGIN_DIR="$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || qmake6 -query QT_INSTALL_PLUGINS)"
QT_PLATFORM_DIR="${QT_PLUGIN_DIR}/platforms"
EXTRA_PLATFORM_PLUGINS="libqoffscreen.so"
wayland_found=0
for candidate in libqwayland.so libqwayland-generic.so libqwayland-egl.so; do
  if [[ -f "${QT_PLATFORM_DIR}/${candidate}" ]]; then
    EXTRA_PLATFORM_PLUGINS="${EXTRA_PLATFORM_PLUGINS};${candidate}"
    wayland_found=1
  fi
done
# Skipping this quietly is how an AppImage ends up xcb-only: it still runs, through XWayland, and
# nothing about the build says otherwise. Install qt6-wayland on the build host.
if [[ "${wayland_found}" -eq 0 ]]; then
  echo "no Qt Wayland platform plugin in ${QT_PLATFORM_DIR}; install qt6-wayland" >&2
  exit 1
fi
export EXTRA_PLATFORM_PLUGINS
export EXTRA_QT_PLUGINS="wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration"
export LD_LIBRARY_PATH="${APPDIR}/usr/lib:${LD_LIBRARY_PATH:-}"
export OUTPUT="${OUTPUT_DIR}/quadcap-${VERSION}-x86_64.AppImage"

# The runners this is built on have no FUSE, so the tool AppImages have to unpack themselves.
export APPIMAGE_EXTRACT_AND_RUN=1

# First pass populates the AppDir. The AppImage itself is produced by the second pass, because a
# plugin directory has to be placed in between.
"${TOOL_DIR}/linuxdeploy" \
  --appdir "${APPDIR}" \
  --executable "${APPDIR}/usr/bin/quadcap" \
  --executable "${APPDIR}/usr/bin/quadcapd" \
  --desktop-file "${APPDIR}/usr/share/applications/quadcap.desktop" \
  --icon-file "${APPDIR}/usr/share/icons/hicolor/scalable/apps/quadcap.svg" \
  --plugin qt \
  --plugin gstreamer

# linuxdeploy-plugin-qt takes wayland-decoration-client and wayland-shell-integration from
# EXTRA_QT_PLUGINS but silently passes over wayland-graphics-integration-client, which is the one
# holding the EGL client buffer integration. Without it Qt reports "Failed to load client buffer
# integration: wayland-egl" and never gets a GL surface, so it is copied in by hand and linuxdeploy
# is asked to resolve what it links against.
GRAPHICS_INTEGRATION="${QT_PLUGIN_DIR}/wayland-graphics-integration-client"
if [[ -d "${GRAPHICS_INTEGRATION}" ]]; then
  mkdir -p "${APPDIR}/usr/plugins"
  cp -r "${GRAPHICS_INTEGRATION}" "${APPDIR}/usr/plugins/"
fi

"${TOOL_DIR}/linuxdeploy" \
  --appdir "${APPDIR}" \
  --deploy-deps-only "${APPDIR}/usr/plugins/wayland-graphics-integration-client" \
  --output appimage

[[ -f "${OUTPUT}" ]] || { echo "no AppImage was produced" >&2; exit 1; }

# Everything the capture graph builds itself out of. A missing plugin does not fail the deployment,
# it fails the app at the moment someone tries to use it, so the bundle is checked against the list
# here instead. glupload in particular lives in gstreamer1.0-gl rather than plugins-base, which is
# easy to leave off a build host and impossible to notice from a successful build.
REQUIRED_PLUGINS=(
  libgstcoreelements   # queue, tee
  libgstvideo4linux2   # v4l2src
  libgstopengl         # glupload, glcolorconvert
  libgstqml6           # qml6glsink
  libgstnvcodec        # nvh265enc
  libgstvideoparsersbad # h265parse
  libgstmatroska       # matroskamux
  libgstmultifile      # splitmuxsink, splitmuxsrc
  libgstvideorate
  libgstalsa
  libgstpipewire
  libgstaudioconvert
  libgstaudioresample
  libgstaudiorate
  libgstaudiomixer
  libgstvolume
  libgstlevel
)
missing=()
for plugin in "${REQUIRED_PLUGINS[@]}"; do
  if [[ ! -f "${APPDIR}/usr/lib/gstreamer-1.0/${plugin}.so" ]]; then
    missing+=("${plugin}")
  fi
done
# Qt loads the Wayland buffer integration from here; without it a Wayland session cannot get a GL
# surface and the window never comes up.
if [[ ! -d "${APPDIR}/usr/plugins/wayland-graphics-integration-client" ]]; then
  missing+=(wayland-graphics-integration-client)
fi
if [[ ${#missing[@]} -gt 0 ]]; then
  rm -f "${OUTPUT}"
  echo "the AppImage is missing: ${missing[*]}" >&2
  echo "install the packages providing them on the build host and try again" >&2
  exit 1
fi

cd "${OUTPUT_DIR}"
sha256sum "$(basename "${OUTPUT}")" >"$(basename "${OUTPUT}").sha256"
