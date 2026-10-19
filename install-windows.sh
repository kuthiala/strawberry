#!/usr/bin/env bash
# ==============================================================================
# install-windows.sh — Build & install the Strawberry music player on Windows.
# ==============================================================================
#
# Runs inside an MSYS2 UCRT64 shell (or start it from cmd/PowerShell with
# install-windows.cmd, which installs MSYS2 first if needed). Builds this fork
# with the same feature set as the official Windows installer PLUS iPod sync,
# which upstream's Windows builds leave out, and installs a self-contained
# copy (no MSYS2 needed at runtime) with a Start Menu shortcut.
#
# Where the pieces come from:
#   * MSYS2 UCRT64 packages: Qt 6, GStreamer (+ the same plugin set as the
#     official installer, incl. Spotify), TagLib, Chromaprint, FFTW, EBU R128,
#     KDSingleApplication, libplist, gdk-pixbuf, ...
#   * Built from source by this script into ${DEPS_ROOT}:
#       - the fork's patched libgpod (github.com/kuthiala/strawberry-libgpod),
#       - getopt-win (the Unicode getopt Strawberry uses on Windows).
#
# Usage:
#   ./install-windows.sh [command] [options]
#
# Commands:
#   deps         Install the MSYS2 packages, build getopt-win and libgpod
#   libgpod      (Re)build only the patched libgpod
#   configure    Run the cmake configure step only
#   build        Configure (if needed) + compile
#   bundle       Assemble a self-contained copy in <build-dir>/dist (Qt and
#                GStreamer plugins + every DLL they need) and audit it
#   install      bundle + copy into the install dir + Start Menu shortcut
#   uninstall    Remove the installed copy and shortcuts (asks about user data)
#   purge        uninstall + user data + build dir + ${DEPS_ROOT}
#   clean        Remove only the build directory
#   status       Show what is currently installed / present
#   doctor       Diagnose the environment
#   all          deps + build + install   (default if no command given)
#   help         Show this help
#
# Options:
#   --build-dir DIR      CMake build dir (default: ./build-windows)
#   --prefix DIR         Install dir (default: %LOCALAPPDATA%\Programs\Strawberry)
#   --build-type TYPE    CMake build type (default: Release)
#   --jobs N             Parallel jobs (default: number of CPUs)
#   --no-bundle          install: copy the existing dist/ without re-bundling
#   --refresh-deps       Re-pull and rebuild libgpod and getopt-win
#   --libgpod-src DIR    Use a local libgpod checkout instead of cloning
#   --desktop-shortcut   Also put a shortcut on the desktop
#   --yes, -y            Don't prompt before destructive actions
#   --verbose, -v        Verbose output (set -x)
#
# Environment overrides: LIBGPOD_REPO, LIBGPOD_REF, EXTRA_CMAKE_ARGS.
#
# License: GPL-3+ (same as the project)
# ==============================================================================

set -Eeuo pipefail

# ------------------------------------------------------------------------------
# Constants
# ------------------------------------------------------------------------------
readonly SCRIPT_NAME="install-windows.sh"
readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

readonly APP_EXE="strawberry.exe"
readonly APP_DISPLAY_NAME="Strawberry"

readonly LIBGPOD_REPO="${LIBGPOD_REPO:-https://github.com/kuthiala/strawberry-libgpod.git}"
readonly LIBGPOD_REF="${LIBGPOD_REF:-master}"

# Same version and checksum as upstream's MSVC and MXE dependency builds.
readonly GETOPT_WIN_VERSION="2.42.0"
readonly GETOPT_WIN_SHA256="3d6ae295260b62af0e2bf2b0287d88e6beeb7a1f7c1f85738c6db4b33e7df13c"
readonly GETOPT_WIN_URL="https://github.com/ludvikjerabek/getopt-win/archive/refs/tags/v${GETOPT_WIN_VERSION}.tar.gz"

# MSYS2 packages, without the ${MINGW_PACKAGE_PREFIX}- prefix.
readonly MINGW_PKGS=(
  toolchain cmake ninja pkgconf ntldd
  qt6-base qt6-tools qt6-imageformats qt6-translations
  glib2 glib-networking
  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly gst-libav gst-plugins-rs
  taglib sqlite3 icu openssl uchardet chromaprint fftw libebur128 sparsehash kdsingleapplication
  libplist gdk-pixbuf2 zlib
)
readonly MSYS_PKGS=( git tar )

# The GStreamer plugins the official Windows installer ships (dist/windows/strawberry.nsi.in).
readonly GST_PLUGINS=(
  adaptivedemux2 aiff apetag app asf asfmux asio audioconvert audiofx audioparsers
  audioresample autodetect bs2b coreelements dash directsound dsd equalizer faac
  faad fdkaac flac gme hls icydemux id3demux isomp4 lame libav matroska
  mpegtsdemux mpg123 musepack ogg openmpt opus pbtypes playback replaygain rtp
  rtpmanager rtsp soup spectrum speex taglib typefindfunctions udp volume vorbis
  wasapi wasapi2 waveform wavenc wavpack wavparse xingmux spotify
)

# Qt plugins, as <subdir>:<file glob>, copied next to strawberry.exe like the official installer does.
readonly QT_PLUGINS=(
  "platforms:qwindows.dll"
  "styles:qmodernwindowsstyle.dll"
  "tls:*.dll"
  "sqldrivers:qsqlite.dll"
  "imageformats:*.dll"
)

# Loaded at runtime with LoadLibrary, so ntldd can't see them.
readonly DLOPENED_DLLS=( "libssl-3*.dll" "libcrypto-3*.dll" "libsoup-3.0-0.dll" )

# Default settings (overridable by flags)
BUILD_DIR="${REPO_ROOT}/build-windows"
INSTALL_PREFIX=""
BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || echo 4)"
DO_BUNDLE=1
REFRESH_DEPS=0
LIBGPOD_SRC=""
DESKTOP_SHORTCUT=0
ASSUME_YES=0
VERBOSE=0

# Set by detect_msys2().
DEPS_ROOT=""
DEPS_PREFIX=""
WINDIR_LOWER=""

# ------------------------------------------------------------------------------
# Logging helpers
# ------------------------------------------------------------------------------
if [[ -t 1 ]]; then
  C_RESET=$'\033[0m'
  C_BOLD=$'\033[1m'
  C_DIM=$'\033[2m'
  C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'
  C_BLUE=$'\033[34m'
  C_CYAN=$'\033[36m'
else
  C_RESET="" ; C_BOLD="" ; C_DIM=""
  C_RED="" ; C_GREEN="" ; C_YELLOW="" ; C_BLUE="" ; C_CYAN=""
fi

log()     { printf "%s[%s]%s %s\n" "${C_DIM}" "$(date +%H:%M:%S)" "${C_RESET}" "$*"; }
info()    { printf "%s==>%s %s\n"  "${C_BLUE}${C_BOLD}" "${C_RESET}" "$*"; }
ok()      { printf "%s  ✓%s %s\n"   "${C_GREEN}"       "${C_RESET}" "$*"; }
warn()    { printf "%s  !%s %s\n"   "${C_YELLOW}"      "${C_RESET}" "$*" >&2; }
error()   { printf "%s  ✗%s %s\n"   "${C_RED}${C_BOLD}" "${C_RESET}" "$*" >&2; }
fatal()   { error "$*"; exit 1; }
step()    { printf "\n%s%s>>>%s %s%s%s\n" "${C_CYAN}" "${C_BOLD}" "${C_RESET}" "${C_CYAN}" "$*" "${C_RESET}"; }

on_err() {
  local exit_code=$?
  local line=${BASH_LINENO[0]:-?}
  error "Command failed (line ${line}, exit ${exit_code})"
  exit "${exit_code}"
}
trap on_err ERR

# ------------------------------------------------------------------------------
# Small utility helpers
# ------------------------------------------------------------------------------
confirm() {
  local prompt="${1:-Continue?}"
  (( ASSUME_YES )) && return 0
  local reply
  read -r -p "${prompt} [y/N] " reply
  [[ "${reply}" =~ ^([yY]|[yY][eE][sS])$ ]]
}

have_cmd() { command -v "$1" >/dev/null 2>&1; }

# Windows path with forward slashes (C:/msys64/...), what native CMake and pkgconf expect.
winpath() { cygpath -m "$1"; }

# Run the MSYS2 environment checks and derive the paths that depend on it.
detect_msys2() {
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) fatal "This script runs inside MSYS2 on Windows. Detected: $(uname -s). On macOS use ./install-macos.sh." ;;
  esac
  case "${MSYSTEM:-}" in
    UCRT64|CLANG64|CLANGARM64) ;;
    MINGW64)
      warn "MINGW64 (msvcrt) works but UCRT64 is the tested environment." ;;
    *)
      fatal "Run this from an MSYS2 UCRT64 shell (MSYSTEM is '${MSYSTEM:-unset}'), or use install-windows.cmd." ;;
  esac
  [[ -n "${MINGW_PREFIX:-}" && -n "${MINGW_PACKAGE_PREFIX:-}" ]] \
    || fatal "MINGW_PREFIX / MINGW_PACKAGE_PREFIX not set; start a login shell (bash -l)."
  have_cmd cygpath || fatal "cygpath not found; this doesn't look like MSYS2."

  # Inside the MSYS2 root, like /opt/strawberry_macos_* on macOS: no spaces in the path, which pkg-config can't handle.
  DEPS_ROOT="/opt/strawberry-install/${MSYSTEM}"
  DEPS_PREFIX="${DEPS_ROOT}/prefix"
  WINDIR_LOWER="$(cygpath -m "${WINDIR:-C:\\Windows}")"
  WINDIR_LOWER="${WINDIR_LOWER,,}"

  if [[ -z "${INSTALL_PREFIX}" ]]; then
    [[ -n "${LOCALAPPDATA:-}" ]] || fatal "LOCALAPPDATA is not set; pass --prefix."
    INSTALL_PREFIX="${LOCALAPPDATA}/Programs/Strawberry"
  fi
}

pkg_name() { echo "${MINGW_PACKAGE_PREFIX}-$1"; }

pkg_installed() { pacman -Q "$1" >/dev/null 2>&1; }

sha256_of() { sha256sum "$1" | awk '{print $1}'; }

# ------------------------------------------------------------------------------
# MSYS2 packages
# ------------------------------------------------------------------------------
ensure_packages() {
  local -a wanted=() missing=()
  local p
  for p in "${MINGW_PKGS[@]}"; do wanted+=("$(pkg_name "${p}")"); done
  wanted+=("${MSYS_PKGS[@]}")
  for p in "${wanted[@]}"; do
    pkg_installed "${p}" || missing+=("${p}")
  done
  if (( ${#missing[@]} == 0 )); then
    ok "All ${#wanted[@]} MSYS2 packages present"
    return 0
  fi
  info "Installing ${#missing[@]} MSYS2 package(s): ${missing[*]}"
  # No -Syu here: updating MSYS2's core packages closes every MSYS2 process, this script included.
  # install-windows.cmd --refresh-deps runs the full update from outside MSYS2.
  if ! pacman -S --needed --noconfirm "${missing[@]}"; then
    error "pacman failed. If it reported 404s, the package database is stale:"
    error "  run 'pacman -Syu' (twice, reopening the shell if asked) or 'install-windows.cmd --refresh-deps', then retry."
    exit 1
  fi
  ok "MSYS2 packages installed"
}

# ------------------------------------------------------------------------------
# getopt-win: Strawberry parses wide-character argv with it on Windows.
# ------------------------------------------------------------------------------
getopt_installed() {
  [[ -f "${DEPS_PREFIX}/lib/cmake/getopt/getoptConfig.cmake" ]] && [[ -f "${DEPS_PREFIX}/include/getopt.h" ]] \
    && compgen -G "${DEPS_PREFIX}/bin/*getopt*.dll" >/dev/null
}

ensure_getopt_win() {
  if getopt_installed && (( ! REFRESH_DEPS )); then
    ok "getopt-win ${GETOPT_WIN_VERSION} present in ${DEPS_PREFIX}"
    return 0
  fi
  local tarball="${DEPS_ROOT}/downloads/getopt-win-${GETOPT_WIN_VERSION}.tar.gz"
  local src="${DEPS_ROOT}/src/getopt-win-${GETOPT_WIN_VERSION}"
  local build="${DEPS_ROOT}/build/getopt-win"
  mkdir -p "$(dirname "${tarball}")" "${DEPS_ROOT}/src"
  if [[ ! -f "${tarball}" ]]; then
    info "Downloading getopt-win ${GETOPT_WIN_VERSION}"
    curl -fL -s -o "${tarball}.partial" "${GETOPT_WIN_URL}"
    mv "${tarball}.partial" "${tarball}"
  fi
  local sum
  sum="$(sha256_of "${tarball}")"
  if [[ "${sum}" != "${GETOPT_WIN_SHA256}" ]]; then
    rm -f "${tarball}"
    fatal "getopt-win checksum mismatch (got ${sum}, expected ${GETOPT_WIN_SHA256}); deleted the download."
  fi
  rm -rf "${src}" "${build}"
  tar -C "${DEPS_ROOT}/src" -xzf "${tarball}"
  info "Building getopt-win"
  cmake -S "${src}" -B "${build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$(winpath "${DEPS_PREFIX}")" \
    -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_TESTING=OFF
  cmake --build "${build}" --parallel "${JOBS}"
  cmake --install "${build}"
  getopt_installed || fatal "getopt-win did not install into ${DEPS_PREFIX}"
  ok "getopt-win ${GETOPT_WIN_VERSION} installed in ${DEPS_PREFIX}"
}

# ------------------------------------------------------------------------------
# The fork's patched libgpod (see .ai/11-macos-dev-loop.md §11.7 for the patches).
# ------------------------------------------------------------------------------
libgpod_src_dir() {
  if [[ -n "${LIBGPOD_SRC}" ]]; then
    echo "${LIBGPOD_SRC}"
  else
    echo "${DEPS_ROOT}/src/strawberry-libgpod"
  fi
}

libgpod_dll() { echo "${DEPS_PREFIX}/bin/libgpod.dll"; }

libgpod_installed() {
  [[ -f "$(libgpod_dll)" ]] && [[ -f "${DEPS_PREFIX}/lib/pkgconfig/libgpod-1.0.pc" ]]
}

# The commit the installed libgpod.dll was built from, so a changed checkout triggers a rebuild.
# Learned on macOS (Bug #12): a rebuilt library that never reaches the app looks exactly like a fix that doesn't work.
libgpod_stamp() { echo "${DEPS_PREFIX}/libgpod.commit"; }

libgpod_source_commit() {
  local src="$1"
  if git -C "${src}" rev-parse HEAD >/dev/null 2>&1; then
    local commit
    commit="$(git -C "${src}" rev-parse HEAD)"
    if [[ -n "$(git -C "${src}" status --porcelain --untracked-files=no 2>/dev/null)" ]]; then
      commit="${commit}-dirty-$(git -C "${src}" diff | sha256sum | cut -c1-12)"
    fi
    echo "${commit}"
  else
    echo "unknown"
  fi
}

fetch_libgpod() {
  local src
  src="$(libgpod_src_dir)"
  if [[ -n "${LIBGPOD_SRC}" ]]; then
    [[ -f "${src}/CMakeLists.txt" ]] || fatal "--libgpod-src ${src} has no CMakeLists.txt"
    ok "Using local libgpod checkout ${src}"
    return 0
  fi
  if [[ ! -d "${src}/.git" ]]; then
    info "Cloning ${LIBGPOD_REPO} (${LIBGPOD_REF})"
    mkdir -p "$(dirname "${src}")"
    git clone --quiet "${LIBGPOD_REPO}" "${src}"
  elif (( REFRESH_DEPS )); then
    info "Updating libgpod checkout"
    git -C "${src}" fetch --quiet origin
  fi
  git -C "${src}" checkout --quiet "${LIBGPOD_REF}"
  if git -C "${src}" symbolic-ref -q HEAD >/dev/null; then
    git -C "${src}" merge --quiet --ff-only "origin/${LIBGPOD_REF}" 2>/dev/null || true
  fi
  ok "libgpod source at $(git -C "${src}" log --oneline -1)"
}

ensure_libgpod() {
  fetch_libgpod
  local src commit
  src="$(libgpod_src_dir)"
  commit="$(libgpod_source_commit "${src}")"
  if libgpod_installed && [[ -f "$(libgpod_stamp)" ]] && [[ "$(cat "$(libgpod_stamp)")" == "${commit}" ]] && (( ! REFRESH_DEPS )); then
    ok "Patched libgpod (${commit:0:12}) already built"
    return 0
  fi
  build_libgpod "${src}" "${commit}"
}

build_libgpod() {
  local src="$1" commit="$2"
  local build="${DEPS_ROOT}/build/libgpod"
  info "Building patched libgpod from ${src}"
  rm -rf "${build}"
  # CMAKE_POLICY_VERSION_MINIMUM: libgpod's cmake_minimum_required predates what CMake 4 accepts.
  cmake -S "${src}" -B "${build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="$(winpath "${DEPS_PREFIX}")"
  grep -q '^#define HAVE_GDKPIXBUF' "${build}/config.h" \
    || fatal "libgpod configured without gdk-pixbuf: iPod cover art would be silently dropped. Is ${MINGW_PACKAGE_PREFIX}-gdk-pixbuf2 installed?"
  cmake --build "${build}" --parallel "${JOBS}"
  cmake --install "${build}"
  libgpod_installed || fatal "libgpod did not install into ${DEPS_PREFIX}"
  # Bug #11 fix marker (checked g_try_realloc): proves this is the fork's libgpod, not a stock one.
  if ! strings "$(libgpod_dll)" | grep -q g_try_realloc; then
    fatal "$(libgpod_dll) lacks the fork's patches (no g_try_realloc). Is LIBGPOD_REPO/--libgpod-src the patched fork?"
  fi
  echo "${commit}" > "$(libgpod_stamp)"
  ok "Patched libgpod installed: $(libgpod_dll)"
}

# ------------------------------------------------------------------------------
# Commands
# ------------------------------------------------------------------------------
cmd_deps() {
  step "Installing build dependencies"
  ensure_packages
  ensure_getopt_win
  ensure_libgpod
  ok "All dependencies are present"
}

cmd_libgpod() {
  step "Building the patched libgpod"
  ensure_packages
  local src
  fetch_libgpod
  src="$(libgpod_src_dir)"
  build_libgpod "${src}" "$(libgpod_source_commit "${src}")"
}

deps_ready() { getopt_installed && libgpod_installed; }

ensure_deps() {
  if deps_ready && (( ! REFRESH_DEPS )); then
    ensure_packages
    # Still rebuild libgpod when its checkout moved on.
    ensure_libgpod
  else
    cmd_deps
  fi
}

# Same switches as the official MSVC/MinGW CI builds (.github/workflows/build.yaml), except GPOD, which upstream turns off on Windows.
# GIO stays off: GLib's Windows volume monitor can't see hot-plugged drives, WindowsDeviceLister does that job.
cmake_configure_args() {
  local enable_spotify="OFF"
  if [[ -f "${MINGW_PREFIX}/lib/gstreamer-1.0/libgstspotify.dll" ]]; then
    enable_spotify="ON"
  fi
  local console="OFF"
  [[ "${BUILD_TYPE}" == "Debug" ]] && console="ON"

  local -a args=(
    -S "${REPO_ROOT}"
    -B "${BUILD_DIR}"
    -G Ninja
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_PREFIX_PATH="$(winpath "${DEPS_PREFIX}")"
    -DUSE_BUNDLE=ON
    -DENABLE_WIN32_CONSOLE="${console}"
    -DENABLE_GIO=OFF
    -DENABLE_AUDIOCD=OFF
    -DENABLE_MTP=OFF
    -DENABLE_GPOD=ON
    -DENABLE_SPOTIFY="${enable_spotify}"
    -DENABLE_QTSPARKLE=OFF
  )
  printf '%s\n' "${args[@]}"
}

# pkg-config search path with the private prefix first; MSYS2 converts the list to Windows form for native programs.
deps_pkg_config_path() {
  echo "${DEPS_PREFIX}/lib/pkgconfig:${MINGW_PREFIX}/lib/pkgconfig:${MINGW_PREFIX}/share/pkgconfig"
}

cmd_configure() {
  step "Configuring CMake build"
  ensure_deps
  mkdir -p "${BUILD_DIR}"

  local -a configure_args=()
  local line
  while IFS= read -r line; do configure_args+=( "${line}" ); done < <(cmake_configure_args)
  if [[ -n "${EXTRA_CMAKE_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    configure_args+=( ${EXTRA_CMAKE_ARGS} )
  fi
  (( VERBOSE )) && info "cmake args: ${configure_args[*]}"

  PKG_CONFIG_PATH="$(deps_pkg_config_path)" cmake "${configure_args[@]}"

  # Refuse to continue without iPod support, that is the point of this build.
  local config_h="${BUILD_DIR}/src/config.h"
  [[ -f "${config_h}" ]] || fatal "${config_h} was not generated"
  grep -q '^#define HAVE_GPOD' "${config_h}" \
    || fatal "CMake configured without iPod support (HAVE_GPOD). Check the 'libgpod' / 'gdk-pixbuf' lines in the CMake summary above."
  ok "CMake configured with iPod support (build dir: ${BUILD_DIR})"
}

cmd_build() {
  step "Building Strawberry"
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmd_configure
  else
    ensure_deps
    ok "Reusing existing CMake configuration in ${BUILD_DIR}"
  fi
  PKG_CONFIG_PATH="$(deps_pkg_config_path)" \
    cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" --parallel "${JOBS}"
  [[ -f "${BUILD_DIR}/${APP_EXE}" ]] || fatal "Build finished but ${BUILD_DIR}/${APP_EXE} does not exist."
  ok "Built ${BUILD_DIR}/${APP_EXE}"
}

dist_dir() { echo "${BUILD_DIR}/dist"; }

# Print the DLLs a set of PE files load, one "<name>|<resolved path or 'not found'>" per line.
# ntldd prints "<name> => <path> (0x<address>)" or "<name> => not found".
dll_deps() {
  ntldd -R "$@" 2>/dev/null | tr -d '\r' | sed -nE \
    -e 's/^[[:space:]]*([^[:space:]]+) => not found[[:space:]]*$/\1|not found/p' \
    -e 's/^[[:space:]]*([^[:space:]]+) => (.+) \(0x[0-9a-fA-F]+\)[[:space:]]*$/\1|\2/p' \
    | sort -u
}

# Lower-case names of the DLLs in the top level of dist/, where Windows looks first.
declare -A DIST_DLLS=()
index_dist_dlls() {
  local dist="$1" f
  DIST_DLLS=()
  while IFS= read -r -d '' f; do
    f="${f##*/}"
    DIST_DLLS["${f,,}"]=1
  done < <(find "${dist}" -maxdepth 1 -type f -iname '*.dll' -print0)
}
dist_has_dll() { [[ -n "${DIST_DLLS["${1,,}"]:-}" ]]; }

# Is this DLL part of Windows (system dir or an API set) rather than something we have to ship?
is_system_dll() {
  local name="$1" path="$2"
  local lower_name="${name,,}"
  case "${lower_name}" in
    api-ms-win-*|ext-ms-win-*) return 0 ;;
  esac
  local lower_path="${path,,}"
  lower_path="${lower_path//\\//}"
  [[ "${lower_path}" == "${WINDIR_LOWER}/"* ]]
}

# Copy every non-system DLL the files in dist/ need into dist/, until nothing new turns up.
bundle_copy_dependencies() {
  local dist="$1"
  local pass=0 copied
  local -a files=()
  while :; do
    pass=$((pass + 1))
    copied=0
    files=()
    while IFS= read -r -d '' f; do files+=("${f}"); done \
      < <(find "${dist}" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
    index_dist_dlls "${dist}"
    local entry name path
    while IFS= read -r entry; do
      name="${entry%%|*}"
      path="${entry#*|}"
      [[ "${path}" == "not found" ]] && continue
      is_system_dll "${name}" "${path}" && continue
      dist_has_dll "${name}" && continue
      cp "$(cygpath -u "${path}")" "${dist}/${name}"
      DIST_DLLS["${name,,}"]=1
      copied=$((copied + 1))
    done < <(dll_deps "${files[@]}")
    log "  dependency pass ${pass}: copied ${copied} DLL(s)"
    (( copied == 0 )) && break
    (( pass >= 10 )) && fatal "DLL dependency copy did not converge after 10 passes"
  done
}

# Every DLL any shipped file loads must be in dist/ or part of Windows; otherwise the installed app only runs on machines with MSYS2.
# Returns the problems through the array named by $2.
bundle_audit() {
  local dist="$1"
  local -n _problems="$2"
  _problems=()
  local -a files=()
  while IFS= read -r -d '' f; do files+=("${f}"); done \
    < <(find "${dist}" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
  index_dist_dlls "${dist}"
  local entry name path
  while IFS= read -r entry; do
    name="${entry%%|*}"
    path="${entry#*|}"
    is_system_dll "${name}" "${path}" && continue
    dist_has_dll "${name}" && continue
    _problems+=("${name} (${path})")
  done < <(dll_deps "${files[@]}")
}

cmd_bundle() {
  step "Bundling a self-contained copy in $(dist_dir)"
  [[ -f "${BUILD_DIR}/${APP_EXE}" ]] || fatal "${BUILD_DIR}/${APP_EXE} not found — run 'build' first."
  have_cmd ntldd || fatal "ntldd not found; run './${SCRIPT_NAME} deps'."

  local dist
  dist="$(dist_dir)"
  # Always from scratch: copying over an old dist/ is how a stale DLL survives a rebuild (macOS Bug #12).
  rm -rf "${dist}"
  mkdir -p "${dist}"

  cp "${BUILD_DIR}/${APP_EXE}" "${dist}/"
  cp "${REPO_ROOT}/COPYING" "${dist}/" 2>/dev/null || true

  # Qt plugins
  local qt_plugins="${MINGW_PREFIX}/share/qt6/plugins"
  local spec subdir pattern f n
  for spec in "${QT_PLUGINS[@]}"; do
    subdir="${spec%%:*}"
    pattern="${spec#*:}"
    mkdir -p "${dist}/${subdir}"
    n=0
    for f in "${qt_plugins}/${subdir}"/${pattern}; do
      [[ -f "${f}" ]] || continue
      cp "${f}" "${dist}/${subdir}/"
      n=$((n + 1))
    done
    (( n > 0 )) || fatal "No Qt plugin matching ${subdir}/${pattern} in ${qt_plugins}"
  done
  ok "Qt plugins copied"

  # Without qt.conf Qt derives its prefix from Qt6Core.dll's location as if it were still in <prefix>/bin, which points outside the bundle.
  printf '[Paths]\nPrefix = .\nPlugins = .\nTranslations = translations\n' > "${dist}/qt.conf"
  mkdir -p "${dist}/translations"
  for f in "${MINGW_PREFIX}"/share/qt6/translations/qt_*.qm "${MINGW_PREFIX}"/share/qt6/translations/qtbase_*.qm; do
    [[ -f "${f}" ]] && cp "${f}" "${dist}/translations/"
  done

  # GStreamer plugins: the official installer's set, in gstreamer-plugins/ where GstStartup looks for them.
  mkdir -p "${dist}/gstreamer-plugins"
  local plugin missing_plugins=()
  for plugin in "${GST_PLUGINS[@]}"; do
    f="${MINGW_PREFIX}/lib/gstreamer-1.0/libgst${plugin}.dll"
    if [[ -f "${f}" ]]; then
      cp "${f}" "${dist}/gstreamer-plugins/"
    else
      missing_plugins+=("${plugin}")
    fi
  done
  if (( ${#missing_plugins[@]} > 0 )); then
    warn "GStreamer plugins not available in MSYS2, skipped: ${missing_plugins[*]}"
  fi
  ok "$(find "${dist}/gstreamer-plugins" -name '*.dll' | wc -l) GStreamer plugins copied"

  # GIO modules (TLS for GStreamer's HTTP sources) in gio-modules/, also where GstStartup looks.
  mkdir -p "${dist}/gio-modules"
  for f in "${MINGW_PREFIX}"/lib/gio/modules/*.dll; do
    [[ -f "${f}" ]] && cp "${f}" "${dist}/gio-modules/"
  done

  # The patched libgpod and getopt-win, from the private prefix.
  for f in "${DEPS_PREFIX}"/bin/*.dll; do
    [[ -f "${f}" ]] && cp "${f}" "${dist}/"
  done
  [[ -f "${dist}/libgpod.dll" ]] || fatal "libgpod.dll missing from ${DEPS_PREFIX}/bin"

  for pattern in "${DLOPENED_DLLS[@]}"; do
    for f in "${MINGW_PREFIX}"/bin/${pattern}; do
      [[ -f "${f}" ]] && cp "${f}" "${dist}/"
    done
  done

  info "Resolving DLL dependencies with ntldd"
  PATH="${DEPS_PREFIX}/bin:${PATH}" bundle_copy_dependencies "${dist}"

  local -a problems=()
  PATH="${DEPS_PREFIX}/bin:${PATH}" bundle_audit "${dist}" problems
  if (( ${#problems[@]} > 0 )); then
    error "These DLLs are needed but not in the bundle:"
    local p
    for p in "${problems[@]}"; do log "  MISSING: ${p}"; done
    fatal "Refusing to call the bundle self-contained."
  fi

  # The bundled libgpod must be the one just built, with the fork's patches.
  if [[ "$(sha256_of "${dist}/libgpod.dll")" != "$(sha256_of "$(libgpod_dll)")" ]]; then
    fatal "Bundled libgpod.dll differs from $(libgpod_dll)"
  fi

  ok "$(find "${dist}" -type f | wc -l) files, $(du -sh "${dist}" | awk '{print $1}'): ${dist} is self-contained"
}

# Kill any running Strawberry so install/uninstall isn't blocked by locked files.
kill_running_app() {
  if tasklist //FI "IMAGENAME eq ${APP_EXE}" //NH 2>/dev/null | grep -qi "${APP_EXE}"; then
    info "Closing running ${APP_DISPLAY_NAME}"
    taskkill //IM "${APP_EXE}" >/dev/null 2>&1 || true
    sleep 2
    if tasklist //FI "IMAGENAME eq ${APP_EXE}" //NH 2>/dev/null | grep -qi "${APP_EXE}"; then
      warn "Strawberry still running — forcing it to close"
      taskkill //F //IM "${APP_EXE}" >/dev/null 2>&1 || true
      sleep 1
    fi
  fi
}

start_menu_dir() { echo "$(cygpath -u "${APPDATA}")/Microsoft/Windows/Start Menu/Programs"; }
desktop_dir() { cygpath -u "$(cygpath -D)"; }

# Create a .lnk shortcut with PowerShell's WScript.Shell COM object.
make_shortcut() {
  local lnk="$1" target="$2"
  local ps_lnk ps_target ps_dir
  ps_lnk="$(cygpath -w "${lnk}")"
  ps_target="$(cygpath -w "${target}")"
  ps_dir="$(cygpath -w "$(dirname "${target}")")"
  # Single quotes are escaped by doubling them in PowerShell string literals.
  ps_lnk="${ps_lnk//\'/\'\'}"
  ps_target="${ps_target//\'/\'\'}"
  ps_dir="${ps_dir//\'/\'\'}"
  powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \
    "\$s = (New-Object -ComObject WScript.Shell).CreateShortcut('${ps_lnk}'); \$s.TargetPath = '${ps_target}'; \$s.WorkingDirectory = '${ps_dir}'; \$s.IconLocation = '${ps_target},0'; \$s.Description = 'Strawberry Music Player'; \$s.Save()" \
    >/dev/null
}

cmd_install() {
  step "Installing ${APP_DISPLAY_NAME} -> ${INSTALL_PREFIX}"

  if (( DO_BUNDLE )); then
    cmd_bundle
  else
    [[ -f "$(dist_dir)/${APP_EXE}" ]] || fatal "$(dist_dir) has no ${APP_EXE}; run without --no-bundle."
    warn "Installing the existing $(dist_dir) without re-bundling (--no-bundle)"
  fi

  kill_running_app

  # A fresh copy every time, so DLLs from an older build can't linger next to the new ones.
  rm -rf "${INSTALL_PREFIX}"
  mkdir -p "${INSTALL_PREFIX}"
  cp -r "$(dist_dir)/." "${INSTALL_PREFIX}/"
  ok "Installed at $(cygpath -w "${INSTALL_PREFIX}")"

  if [[ -n "${APPDATA:-}" ]]; then
    make_shortcut "$(start_menu_dir)/${APP_DISPLAY_NAME}.lnk" "${INSTALL_PREFIX}/${APP_EXE}" \
      && ok "Start Menu shortcut created" \
      || warn "Could not create the Start Menu shortcut"
  fi
  if (( DESKTOP_SHORTCUT )); then
    make_shortcut "$(desktop_dir)/${APP_DISPLAY_NAME}.lnk" "${INSTALL_PREFIX}/${APP_EXE}" \
      && ok "Desktop shortcut created" \
      || warn "Could not create the desktop shortcut"
  fi
  log "Launch from the Start Menu, or:  \"$(cygpath -w "${INSTALL_PREFIX}/${APP_EXE}")\""
}

# User data: settings live in the registry (QSettings native format), the collection database and caches under %LOCALAPPDATA%.
user_data_paths() {
  [[ -n "${LOCALAPPDATA:-}" ]] && echo "$(cygpath -u "${LOCALAPPDATA}")/Strawberry"
  [[ -n "${APPDATA:-}" ]] && echo "$(cygpath -u "${APPDATA}")/Strawberry"
  return 0
}
readonly USER_REGISTRY_KEY='HKCU\Software\Strawberry'

remove_user_data() {
  local p
  while IFS= read -r p; do
    [[ -z "${p}" ]] && continue
    if [[ -e "${p}" ]]; then
      rm -rf "${p}"
      ok "Removed $(cygpath -w "${p}")"
    fi
  done < <(user_data_paths)
  if reg query "${USER_REGISTRY_KEY}" >/dev/null 2>&1; then
    reg delete "${USER_REGISTRY_KEY}" //f >/dev/null && ok "Removed registry key ${USER_REGISTRY_KEY}"
  fi
}

cmd_uninstall() {
  step "Uninstalling ${APP_DISPLAY_NAME}"
  kill_running_app

  if [[ -d "${INSTALL_PREFIX}" ]]; then
    if confirm "Remove $(cygpath -w "${INSTALL_PREFIX}")?"; then
      rm -rf "${INSTALL_PREFIX}"
      ok "Removed $(cygpath -w "${INSTALL_PREFIX}")"
    else
      warn "Skipped removing ${INSTALL_PREFIX}"
    fi
  else
    ok "${INSTALL_PREFIX} not present — nothing to uninstall"
  fi

  local -a shortcuts=( "$(desktop_dir)/${APP_DISPLAY_NAME}.lnk" )
  [[ -n "${APPDATA:-}" ]] && shortcuts+=( "$(start_menu_dir)/${APP_DISPLAY_NAME}.lnk" )
  local lnk
  for lnk in "${shortcuts[@]}"; do
    if [[ -f "${lnk}" ]]; then
      rm -f "${lnk}"
      ok "Removed shortcut $(cygpath -w "${lnk}")"
    fi
  done

  if confirm "Also remove settings, the collection database and caches?"; then
    remove_user_data
  else
    log "User data preserved. Use './${SCRIPT_NAME} purge' to remove it too."
  fi
}

cmd_clean() {
  step "Cleaning build directory"
  if [[ -d "${BUILD_DIR}" ]]; then
    rm -rf "${BUILD_DIR}"
    ok "Removed ${BUILD_DIR}"
  else
    ok "${BUILD_DIR} does not exist"
  fi
}

cmd_purge() {
  step "Purging everything (app + user data + build dir + ${DEPS_ROOT})"
  ASSUME_YES=1 cmd_uninstall
  remove_user_data
  cmd_clean
  if [[ -d "${DEPS_ROOT}" ]]; then
    rm -rf "${DEPS_ROOT}"
    ok "Removed ${DEPS_ROOT} (libgpod + getopt-win builds)"
  fi
  log "MSYS2 packages were left installed; remove them with pacman -Rns if you no longer need them."
  ok "System restored to a clean state"
}

cmd_status() {
  step "Status"
  printf "  %-24s %s\n" "Repo root:"       "${REPO_ROOT}"
  printf "  %-24s %s\n" "MSYS2 environment:" "${MSYSTEM} (${MINGW_PREFIX})"
  printf "  %-24s %s\n" "Build dir:"       "${BUILD_DIR}"
  printf "  %-24s %s\n" "Build type:"      "${BUILD_TYPE}"
  printf "  %-24s %s\n" "Parallel jobs:"   "${JOBS}"
  printf "  %-24s %s\n" "Private deps:"    "${DEPS_PREFIX}"
  printf "  %-24s %s\n" "Install dir:"     "$(cygpath -w "${INSTALL_PREFIX}")"
  echo
  getopt_installed && ok "getopt-win built" || warn "getopt-win not built (run './${SCRIPT_NAME} deps')"
  if libgpod_installed; then
    ok "Patched libgpod built ($(cat "$(libgpod_stamp)" 2>/dev/null | cut -c1-12))"
  else
    warn "Patched libgpod not built (run './${SCRIPT_NAME} deps')"
  fi
  [[ -f "${BUILD_DIR}/CMakeCache.txt" ]] && ok "Build dir configured" || warn "Build dir not configured"
  [[ -f "${BUILD_DIR}/${APP_EXE}" ]] && ok "Built: ${BUILD_DIR}/${APP_EXE}" || warn "Not built"
  [[ -f "$(dist_dir)/${APP_EXE}" ]] && ok "Bundled: $(dist_dir)" || warn "Not bundled"
  [[ -f "${INSTALL_PREFIX}/${APP_EXE}" ]] && ok "Installed: $(cygpath -w "${INSTALL_PREFIX}")" || warn "Not installed"
}

cmd_doctor() {
  step "Environment doctor"
  local issues=0
  check_cmd() {
    if have_cmd "$1"; then
      ok "$1 -> $(command -v "$1")"
    else
      error "$1 not in PATH"
      issues=$((issues + 1))
    fi
  }
  log "MSYSTEM=${MSYSTEM} MINGW_PREFIX=${MINGW_PREFIX}"
  check_cmd pacman
  check_cmd git
  check_cmd curl
  check_cmd tar
  check_cmd cmake
  check_cmd ninja
  check_cmd pkg-config
  check_cmd ntldd
  check_cmd powershell.exe

  local p missing=0
  for p in "${MINGW_PKGS[@]}"; do
    if ! pkg_installed "$(pkg_name "${p}")"; then
      error "package $(pkg_name "${p}") missing"
      missing=$((missing + 1))
    fi
  done
  (( missing == 0 )) && ok "All ${#MINGW_PKGS[@]} MSYS2 packages installed"
  issues=$((issues + missing))

  if [[ "$(command -v cmake)" != "${MINGW_PREFIX}/bin/cmake" ]]; then
    warn "cmake is $(command -v cmake), not ${MINGW_PREFIX}/bin/cmake; another CMake earlier in PATH can pick the wrong compiler."
  fi
  deps_ready && ok "Private deps ready in ${DEPS_PREFIX}" || warn "Private deps not built yet (run './${SCRIPT_NAME} deps')"

  if (( issues > 0 )); then
    error "${issues} issue(s) detected — run './${SCRIPT_NAME} deps' to fix."
    exit 1
  fi
  ok "Environment looks healthy."
}

cmd_all() {
  cmd_deps
  cmd_build
  cmd_install
  step "Done"
  log "Reset to a clean state:  ./${SCRIPT_NAME} purge -y"
}

cmd_help() {
  sed -n '2,52p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# ------------------------------------------------------------------------------
# Argument parsing
# ------------------------------------------------------------------------------
parse_args() {
  COMMAND=""
  while (( $# > 0 )); do
    case "$1" in
      deps|libgpod|configure|build|bundle|install|uninstall|purge|clean|status|doctor|all|help)
        if [[ -z "${COMMAND}" ]]; then
          COMMAND="$1"
        else
          fatal "Multiple commands given: '${COMMAND}' and '$1'."
        fi
        shift
        ;;
      --build-dir)        BUILD_DIR="$2";          shift 2 ;;
      --build-dir=*)      BUILD_DIR="${1#*=}";     shift   ;;
      --prefix)           INSTALL_PREFIX="$2";     shift 2 ;;
      --prefix=*)         INSTALL_PREFIX="${1#*=}"; shift  ;;
      --build-type)       BUILD_TYPE="$2";         shift 2 ;;
      --build-type=*)     BUILD_TYPE="${1#*=}";    shift   ;;
      --jobs|-j)          JOBS="$2";               shift 2 ;;
      --jobs=*)           JOBS="${1#*=}";          shift   ;;
      --no-bundle)        DO_BUNDLE=0;             shift   ;;
      --refresh-deps)     REFRESH_DEPS=1;          shift   ;;
      --libgpod-src)      LIBGPOD_SRC="$2";        shift 2 ;;
      --libgpod-src=*)    LIBGPOD_SRC="${1#*=}";   shift   ;;
      --desktop-shortcut) DESKTOP_SHORTCUT=1;      shift   ;;
      --yes|-y)           ASSUME_YES=1;            shift   ;;
      --verbose|-v)       VERBOSE=1;               shift   ;;
      -h|--help)          COMMAND="help";          shift   ;;
      --)                 shift; break ;;
      -*)                 fatal "Unknown option: $1 (try --help)" ;;
      *)                  fatal "Unexpected argument: $1 (try --help)" ;;
    esac
  done
  COMMAND="${COMMAND:-all}"
  (( VERBOSE )) && set -x
  return 0
}

# Accept C:\... and C:/... as well as MSYS2 paths; relative paths are relative to the repo.
normalize_paths() {
  case "${BUILD_DIR}" in
    /*|[A-Za-z]:*) BUILD_DIR="$(cygpath -u "${BUILD_DIR}")" ;;
    *)             BUILD_DIR="${REPO_ROOT}/${BUILD_DIR#./}" ;;
  esac
  [[ -n "${INSTALL_PREFIX}" ]] && INSTALL_PREFIX="$(cygpath -u "${INSTALL_PREFIX}")"
  [[ -n "${LIBGPOD_SRC}" ]] && LIBGPOD_SRC="$(cygpath -u "${LIBGPOD_SRC}")"
  return 0
}

main() {
  parse_args "$@"
  [[ "${COMMAND}" == "help" ]] && { cmd_help; return 0; }
  detect_msys2
  normalize_paths
  case "${COMMAND}" in
    deps)      cmd_deps ;;
    libgpod)   cmd_libgpod ;;
    configure) cmd_configure ;;
    build)     cmd_build ;;
    bundle)    cmd_bundle ;;
    install)   cmd_install ;;
    uninstall) cmd_uninstall ;;
    purge)     cmd_purge ;;
    clean)     cmd_clean ;;
    status)    cmd_status ;;
    doctor)    cmd_doctor ;;
    all)       cmd_all ;;
    *)         fatal "Unknown command: ${COMMAND}" ;;
  esac
}

main "$@"
