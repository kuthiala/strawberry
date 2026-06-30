#!/usr/bin/env bash
# ==============================================================================
# install-macos.sh — Build & install the Strawberry music player on macOS.
# ==============================================================================
#
# Designed for macOS Sequoia (15.x) on both Intel (x86_64) and Apple Silicon
# (arm64). Idempotent, robust, and quick — designed so you can install,
# uninstall, and reinstall to verify changes.
#
# **Feature parity with the official sponsor DMG**: this script uses the
# same curated dependency tarball that the upstream CI consumes
# (https://github.com/strawberrymusicplayer/strawberry-macos-dependencies[-legacy])
# instead of trying to glue together Homebrew formulas. That gives us
# everything — libgpod (iPod), sparsehash, Sparkle auto-updater, qtsparkle,
# KDSingleApplication, macdeploycheck, the matching Qt/GStreamer/TagLib
# versions, etc. — exactly as the official build does it.
#
# Usage:
#   ./install-macos.sh [command] [options]
#
# Commands (run with --help for full details):
#   deps         Download + extract the strawberry-macos-dependencies tarball
#                into /opt (and install minimal brew build tools)
#   configure    Run cmake configure step only
#   build        Configure (if needed) + compile
#   bundle       Run `make deploy` (macdeployqt + bundles dylibs/plugins)
#   install      Copy strawberry.app into the Applications folder
#   uninstall    Remove the installed strawberry.app (keeps user data + tarball)
#   purge        uninstall + remove user prefs + build dir + cached tarball
#                + /opt/strawberry_macos_<arch>_release tree
#   clean        Remove only the build directory
#   status       Show what is currently installed / present
#   doctor       Diagnose the environment (deps, paths, versions)
#   all          deps + build + bundle + install   (default if no command given)
#   help         Show full usage
#
# Common options:
#   --build-dir DIR     CMake build dir (default: ./build)
#   --prefix DIR        Where to install the .app (default: /Applications)
#   --build-type TYPE   CMake build type (default: Release)
#   --jobs N            Parallel jobs (default: number of CPUs)
#   --no-bundle         Skip the macdeployqt bundling step (faster dev loop)
#   --refresh-deps      Re-download the deps tarball even if cached
#   --yes, -y           Don't prompt before destructive actions
#   --verbose, -v       Verbose output (set -x)
#
# Author: scaffolded for the strawberry fork @ kuthiala/strawberry
# License: GPL-3+ (same as the project)
# ==============================================================================

set -Eeuo pipefail

# ------------------------------------------------------------------------------
# Constants
# ------------------------------------------------------------------------------
readonly SCRIPT_NAME="install-macos.sh"
readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

readonly APP_NAME="strawberry.app"
readonly APP_DISPLAY_NAME="Strawberry"
readonly APP_BUNDLE_ID="org.strawberrymusicplayer.strawberry"

# Architecture-dependent constants are derived in detect_arch() below.
# Intel Macs use the *-legacy* tarball repo; Apple Silicon uses the main one.
ARCH=""
DEPS_REPO=""
DEPS_PREFIX=""
DEPS_TARBALL_NAME=""
DEPS_TARBALL_URL=""

# Local cache for downloaded artefacts (tarball, etc.)
readonly INSTALL_CACHE_DIR="${HOME}/Library/Caches/strawberry-install"

# Minimal Homebrew packages we still need (build tools — NOT runtime libs).
# Everything else comes from the upstream deps tarball.
readonly BREW_PKGS_BUILDTOOLS=(
  cmake     # CMake ≥ 3.13
  pkgconf   # provides pkg-config
)

# Default settings (overridable by flags)
BUILD_DIR="${REPO_ROOT}/build"
INSTALL_PREFIX="/Applications"
BUILD_TYPE="Release"
JOBS="$(/usr/sbin/sysctl -n hw.ncpu 2>/dev/null || echo 4)"
DO_BUNDLE=1
REFRESH_DEPS=0
ASSUME_YES=0
VERBOSE=0

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

require_macos() {
  if [[ "$(uname -s)" != "Darwin" ]]; then
    fatal "This script only runs on macOS. Detected: $(uname -s)"
  fi
}

detect_macos_version() {
  if ! have_cmd sw_vers; then
    warn "sw_vers not available; cannot verify macOS version"
    return
  fi
  local product_version major
  product_version="$(sw_vers -productVersion)"
  major="${product_version%%.*}"
  log "macOS ${product_version} ($(uname -m))"
  if [[ "${major}" -lt 13 ]]; then
    fatal "macOS 13 (Ventura) or newer is required (Qt 6 + modern toolchains)."
  fi
  if [[ "${major}" -lt 15 ]]; then
    warn "Script tuned for macOS Sequoia (15.x); continuing on macOS ${product_version}."
  fi
}

# Populate ARCH and DEPS_* based on uname.
detect_arch() {
  ARCH="$(uname -m)"
  case "${ARCH}" in
    x86_64)
      DEPS_REPO="strawberrymusicplayer/strawberry-macos-dependencies-legacy"
      ;;
    arm64)
      DEPS_REPO="strawberrymusicplayer/strawberry-macos-dependencies"
      ;;
    *)
      fatal "Unsupported architecture: ${ARCH}"
      ;;
  esac
  DEPS_PREFIX="/opt/strawberry_macos_${ARCH}_release"
  DEPS_TARBALL_NAME="strawberry-macos-${ARCH}-release.tar.xz"
  DEPS_TARBALL_URL="https://github.com/${DEPS_REPO}/releases/latest/download/${DEPS_TARBALL_NAME}"
}

# ------------------------------------------------------------------------------
# Homebrew — only used for cmake & pkg-config (build tools).
# We do NOT install runtime libs through brew; the deps tarball has all of them.
# ------------------------------------------------------------------------------
detect_brew_prefix() {
  case "$(uname -m)" in
    arm64) echo "/opt/homebrew" ;;
    *)     echo "/usr/local"   ;;
  esac
}

BREW_PREFIX=""

ensure_brew() {
  if have_cmd brew; then
    BREW_PREFIX="$(brew --prefix)"
    ok "Homebrew found at ${BREW_PREFIX}"
  else
    info "Homebrew not found — installing (cmake + pkgconf require it)"
    if ! confirm "Install Homebrew now? (requires network & sudo)"; then
      fatal "Homebrew is required for the cmake & pkgconf build tools."
    fi
    /bin/bash -c \
      "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    if ! have_cmd brew; then
      export PATH="$(detect_brew_prefix)/bin:${PATH}"
      have_cmd brew || fatal "Homebrew installation appears to have failed."
    fi
    BREW_PREFIX="$(brew --prefix)"
    ok "Installed Homebrew at ${BREW_PREFIX}"
  fi
  export BREW_PREFIX
  export PATH="${BREW_PREFIX}/bin:${BREW_PREFIX}/sbin:${PATH}"
}

brew_installed() { brew ls --versions "$1" >/dev/null 2>&1; }

ensure_build_tools() {
  ensure_brew
  local -a missing=()
  local pkg
  for pkg in "${BREW_PKGS_BUILDTOOLS[@]}"; do
    if brew_installed "${pkg}"; then
      ok "build tool ${pkg}: present"
    else
      missing+=("${pkg}")
    fi
  done
  if (( ${#missing[@]} > 0 )); then
    info "Installing build tools: ${missing[*]}"
    HOMEBREW_NO_INSTALL_CLEANUP=1 HOMEBREW_NO_ENV_HINTS=1 \
      brew install "${missing[@]}"
  fi
}

# ------------------------------------------------------------------------------
# Strawberry macOS dependencies tarball: download + extract to /opt.
# ------------------------------------------------------------------------------
deps_tarball_cache_path() { echo "${INSTALL_CACHE_DIR}/${DEPS_TARBALL_NAME}"; }

# Is the tarball already extracted into /opt?
deps_installed() {
  [[ -d "${DEPS_PREFIX}" ]] \
    && [[ -x "${DEPS_PREFIX}/bin/macdeployqt" ]] \
    && [[ -d "${DEPS_PREFIX}/lib/cmake" ]]
}

ensure_strawberry_macos_deps() {
  if deps_installed && (( ! REFRESH_DEPS )); then
    ok "Strawberry macOS dependencies already at ${DEPS_PREFIX}"
    return 0
  fi

  local cache_path
  cache_path="$(deps_tarball_cache_path)"
  mkdir -p "${INSTALL_CACHE_DIR}"

  if (( REFRESH_DEPS )) && [[ -f "${cache_path}" ]]; then
    info "Removing cached tarball (--refresh-deps)"
    rm -f "${cache_path}"
  fi

  if [[ ! -f "${cache_path}" ]]; then
    info "Downloading ${DEPS_TARBALL_NAME} (~100 MB) from ${DEPS_REPO}"
    log "  URL: ${DEPS_TARBALL_URL}"
    # -L follow redirects, -f fail on HTTP error, -# progress bar, -C - resume.
    if [[ -t 1 ]]; then
      curl -fL -# -o "${cache_path}.partial" "${DEPS_TARBALL_URL}"
    else
      curl -fL -s -o "${cache_path}.partial" "${DEPS_TARBALL_URL}"
    fi
    mv "${cache_path}.partial" "${cache_path}"
    ok "Cached at ${cache_path}"
  else
    ok "Tarball already cached at ${cache_path}"
  fi

  info "Extracting tarball to / (requires sudo — installs into ${DEPS_PREFIX})"
  if [[ -d "${DEPS_PREFIX}" ]]; then
    log "  Removing previous ${DEPS_PREFIX}"
    sudo rm -rf "${DEPS_PREFIX}"
  fi
  sudo tar -C / -xf "${cache_path}"

  deps_installed \
    || fatal "Extraction did not produce ${DEPS_PREFIX} or it lacks macdeployqt."
  ok "Strawberry macOS dependencies installed at ${DEPS_PREFIX}"
}

# ------------------------------------------------------------------------------
# Commands
# ------------------------------------------------------------------------------
cmd_deps() {
  step "Installing build dependencies"
  ensure_build_tools
  ensure_strawberry_macos_deps
  ok "All dependencies are present"
}

# Build the CMake configure arguments. Mirrors the official CI configure
# (see .github/workflows/build.yml — build-macos-public job).
cmake_configure_args() {
  # Conditionally enable Spotify based on whether the gst plugin shipped
  # with the tarball (matches the CI's auto-detect).
  local enable_spotify="OFF"
  if [[ -f "${DEPS_PREFIX}/lib/gstreamer-1.0/libgstspotify.dylib" ]]; then
    enable_spotify="ON"
  fi

  local -a args=(
    -S "${REPO_ROOT}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_PREFIX_PATH="${DEPS_PREFIX}/lib/cmake"
    -DUSE_BUNDLE=ON
    -DICU_ROOT="${DEPS_PREFIX}"
    -DARCH="${ARCH}"
    -DENABLE_SPOTIFY="${enable_spotify}"
    -DENABLE_SPARKLE=ON
    -DENABLE_QTSPARKLE=OFF
  )
  printf '%s\n' "${args[@]}"
}

cmd_configure() {
  step "Configuring CMake build (with strawberry-macos-dependencies)"
  ensure_build_tools
  ensure_strawberry_macos_deps
  mkdir -p "${BUILD_DIR}"

  local -a configure_args
  configure_args=()
  while IFS= read -r line; do configure_args+=( "${line}" ); done < <(cmake_configure_args)
  (( VERBOSE )) && info "cmake args: ${configure_args[*]}"

  # CRITICAL: PKG_CONFIG_LIBDIR replaces (rather than augments) the default
  # search path. We deliberately set it to ONLY the tarball's pkgconfig dir
  # so that pkg-config never finds a Homebrew formula by accident — that
  # would risk linking against mismatched versions at runtime.
  PATH="${DEPS_PREFIX}/bin:${PATH}" \
  PKG_CONFIG_LIBDIR="${DEPS_PREFIX}/lib/pkgconfig" \
  PKG_CONFIG_PATH="${DEPS_PREFIX}/lib/pkgconfig" \
  LDFLAGS="-L${DEPS_PREFIX}/lib -Wl,-rpath,${DEPS_PREFIX}/lib" \
    cmake "${configure_args[@]}"
  ok "CMake configured (build dir: ${BUILD_DIR})"
}

cmd_build() {
  step "Building Strawberry"
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmd_configure
  else
    ensure_build_tools
    ensure_strawberry_macos_deps
    ok "Reusing existing CMake configuration in ${BUILD_DIR}"
  fi
  # Keep the same PATH/LDFLAGS available for the build (compiler RPATH etc.)
  PATH="${DEPS_PREFIX}/bin:${PATH}" \
    cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" --parallel "${JOBS}"
  if [[ ! -d "${BUILD_DIR}/${APP_NAME}" ]]; then
    fatal "Build finished but ${BUILD_DIR}/${APP_NAME} does not exist."
  fi
  ok "Built ${BUILD_DIR}/${APP_NAME}"
}

cmd_bundle() {
  step "Bundling dependencies into ${APP_NAME} (macdeployqt + gst plugins)"
  ensure_build_tools
  ensure_strawberry_macos_deps

  [[ -d "${BUILD_DIR}/${APP_NAME}" ]] \
    || fatal "${BUILD_DIR}/${APP_NAME} not found — run 'build' first."

  # All four env vars are required by dist/macos/macgstcopy.sh; the deploy
  # target then calls macdeployqt to gather Qt frameworks + plugins.
  local gio_extra_modules="${DEPS_PREFIX}/lib/gio/modules"
  local gst_plugin_scanner="${DEPS_PREFIX}/libexec/gstreamer-1.0/gst-plugin-scanner"
  local gst_plugin_path="${DEPS_PREFIX}/lib/gstreamer-1.0"
  local libsoup_lib="${DEPS_PREFIX}/lib/libsoup-3.0.0.dylib"

  [[ -d "${gio_extra_modules}"   ]] || fatal "Missing ${gio_extra_modules}"
  [[ -x "${gst_plugin_scanner}"  ]] || fatal "Missing ${gst_plugin_scanner}"
  [[ -d "${gst_plugin_path}"     ]] || fatal "Missing ${gst_plugin_path}"
  [[ -e "${libsoup_lib}"         ]] || warn  "Missing ${libsoup_lib} (HTTP streaming via libsoup may fail)"

  PATH="${DEPS_PREFIX}/bin:${PATH}" \
  GIO_EXTRA_MODULES="${gio_extra_modules}" \
  GST_PLUGIN_SCANNER="${gst_plugin_scanner}" \
  GST_PLUGIN_PATH="${gst_plugin_path}" \
  LIBSOUP_LIBRARY_PATH="${libsoup_lib}" \
    cmake --build "${BUILD_DIR}" --target deploy

  # Ad-hoc codesign every dylib + the app bundle. macdeployqt does most of
  # this for us, but it skips a handful that the official CI signs by hand
  # (libbrotli*, libpng16, libfreetype, libsoup, libnghttp2, libpsl, etc.)
  # using an Apple Developer ID. Without that ID we sign with "-" (ad-hoc),
  # which is enough to satisfy macOS Catalina+'s requirement that every
  # loaded mach-o be signed.
  codesign_bundle_adhoc "${BUILD_DIR}/${APP_NAME}"

  # ----------------------------------------------------------------------
  # The CMake `deploy` target fires `macdeployqt` twice in parallel (main
  # pass + recursive `-executable=gst-plugin-scanner` pass). They mutate
  # the same files via `install_name_tool`, which renames-then-rewrites,
  # so the two passes race on a handful of files and one pass fails with
  # `cannot rename ...XXXXXX (No such file or directory)`. The losing
  # pass's edits are lost on whichever files it touched last.
  #
  # Empirically the survivors are usually only a handful (5-ish per run)
  # and the second pass *usually* cleans them up — but "usually" once
  # leaked a crashing .app into /Applications (see
  # `.ai/11-macos-dev-loop.md` §11.13 row 7). The fix is to audit + repair
  # the residual files ourselves so the `is self-contained` line is
  # actually true when we print it.
  # ----------------------------------------------------------------------
  local -a dirty_files=()
  bundle_collect_dirty "${BUILD_DIR}/${APP_NAME}" dirty_files
  if (( ${#dirty_files[@]} > 0 )); then
    warn "${#dirty_files[@]} file(s) still reference ${DEPS_PREFIX}/lib after macdeployqt:"
    local f
    for f in "${dirty_files[@]}"; do log "  DIRTY: ${f}"; done
    info "Repairing residual /opt references in-place (install_name_tool + ad-hoc resign)"
    bundle_repair_dirty "${BUILD_DIR}/${APP_NAME}" "${dirty_files[@]}"

    dirty_files=()
    bundle_collect_dirty "${BUILD_DIR}/${APP_NAME}" dirty_files
    if (( ${#dirty_files[@]} > 0 )); then
      error "Bundle still dirty after repair pass:"
      for f in "${dirty_files[@]}"; do log "  DIRTY: ${f}"; done
      fatal "Refusing to mark bundle self-contained; re-run './${SCRIPT_NAME} bundle' or patch manually."
    fi
    ok "Residual /opt references repaired"
  fi

  # ----------------------------------------------------------------------
  # Stale-content audit. `macdeployqt` only copies a lib into the bundle
  # when no file of that name is already there -- it does NOT compare
  # timestamps or hashes. So if the user (or one of our /opt-patching
  # workflows) rebuilds e.g. `${DEPS_PREFIX}/lib/libgpod.dylib`, the older
  # bytes already in `Contents/Frameworks/libgpod.dylib` survive every
  # subsequent bundle pass, silently shadowing the fix. The runtime
  # behaves like the pre-fix lib even though the source on disk is new.
  #
  # This bit us once with the pack_RGB_565 overflow-guard fix in libgpod;
  # see `.ai/11-macos-dev-loop.md` §11.13 row 9.
  # ----------------------------------------------------------------------
  local -a stale_files=()
  bundle_collect_stale "${BUILD_DIR}/${APP_NAME}" stale_files
  if (( ${#stale_files[@]} > 0 )); then
    warn "${#stale_files[@]} bundled file(s) differ from their ${DEPS_PREFIX}/lib source:"
    local s
    for s in "${stale_files[@]}"; do log "  STALE: ${s%%|*}"; done
    info "Refreshing stale bundled libs from ${DEPS_PREFIX}/lib (cp + install_name_tool + ad-hoc resign)"
    bundle_repair_stale "${BUILD_DIR}/${APP_NAME}" "${stale_files[@]}"

    stale_files=()
    bundle_collect_stale "${BUILD_DIR}/${APP_NAME}" stale_files
    if (( ${#stale_files[@]} > 0 )); then
      error "Bundle still stale after refresh pass:"
      for s in "${stale_files[@]}"; do log "  STALE: ${s%%|*}"; done
      fatal "Refusing to mark bundle self-contained; re-run './${SCRIPT_NAME} bundle' or patch manually."
    fi
    ok "Stale bundled libs refreshed"
  fi

  ok "${BUILD_DIR}/${APP_NAME} is self-contained"
}

# Populate the named array variable (passed as $2) with every Mach-O file
# inside the .app at $1 that still references ${DEPS_PREFIX}/lib in any of
# its load commands. This is the §11.3 full-bundle audit, lifted out so
# `cmd_bundle` can run it on itself and `cmd_install` can use it as a
# pre-flight check.
#
# We deliberately scan the binary, every *.dylib / *.so, and any Mach-O
# executable under PlugIns/ (gst-plugin-scanner etc.). That covers every
# loader-path origin the launching app actually walks.
bundle_collect_dirty() {
  local bundle="$1"
  local -n _out="$2"
  _out=()
  local f
  while IFS= read -r -d '' f; do
    if otool -L "${f}" 2>/dev/null | grep -q "${DEPS_PREFIX}/lib"; then
      _out+=("${f}")
    fi
  done < <(find "${bundle}/Contents" \
              \( -name "*.dylib" -o -name "*.so" -o -name "strawberry" \
                 -o -name "gst-plugin-scanner" -o -name "macdeploycheck" \) \
              -type f -print0 2>/dev/null)
}

# For each file in $2..$N, rewrite every /opt-prefixed load command to the
# bundle-relative @loader_path equivalent, then re-codesign. The relative
# path differs between Frameworks/ (same dir) and PlugIns/<subdir>/ (two
# levels up); we derive it from where the file sits in the bundle.
bundle_repair_dirty() {
  local bundle="$1"; shift
  local file ref new bad rel depth
  for file in "$@"; do
    # How many "../" hops from this file's directory to Contents/Frameworks?
    # Contents/Frameworks/foo.dylib   -> same dir   -> @loader_path/<lib>
    # Contents/PlugIns/x/foo.dylib    -> two up     -> ../../Frameworks/<lib>
    # Contents/PlugIns/foo.dylib      -> one up     -> ../Frameworks/<lib>
    # Contents/MacOS/strawberry       -> one up     -> ../Frameworks/<lib>
    rel="${file#${bundle}/Contents/}"
    case "${rel}" in
      Frameworks/*)        depth="" ;;
      PlugIns/*/*)         depth="../../Frameworks/" ;;
      PlugIns/*)           depth="../Frameworks/" ;;
      MacOS/*)             depth="../Frameworks/" ;;
      *)                   depth="../Frameworks/" ;;
    esac

    # Iterate every /opt/.../lib/<libname> load command on this file
    while IFS= read -r bad; do
      [[ -z "${bad}" ]] && continue
      ref="$(basename "${bad}")"
      new="@loader_path/${depth}${ref}"
      log "  patch ${file##${bundle}/}: ${bad} -> ${new}"
      install_name_tool -change "${bad}" "${new}" "${file}" 2>/dev/null \
        || warn "  install_name_tool failed for ${file} (${bad})"
    done < <(otool -L "${file}" 2>/dev/null \
               | awk -v p="${DEPS_PREFIX}/lib" '$1 ~ p {print $1}')

    # install_name_tool invalidates the codesignature; re-sign in place.
    codesign --force --sign - --timestamp=none "${file}" 2>/dev/null \
      || warn "  codesign failed for ${file}"
  done
}

# Populate the named array variable (passed as $2) with every Mach-O file
# inside the .app at $1 whose basename ALSO exists at
# `${DEPS_PREFIX}/lib/<basename>` (or
# `${DEPS_PREFIX}/lib/gstreamer-1.0/<basename>` for GStreamer plugins) AND
# whose contents differ from that source by SHA-256.
#
# Why this matters: `macdeployqt` only copies a dylib into the bundle when
# the bundle doesn't already have a copy of the same name. It does NOT
# compare timestamps or hashes. So once a lib is bundled, subsequent
# `cmake --build`-driven rebuilds of `${DEPS_PREFIX}/lib/<x>.dylib` (e.g.
# when the user rebuilds a patched libgpod, libtag, libgstreamer, …) get
# silently shadowed by the older bundled bytes. Symptom: the runtime
# behaviour reflects the OLD library even though the source on disk has
# been fixed.
#
# This caught us once: a patched libgpod with the `pack_RGB_565` overflow
# guard fix was sitting at ${DEPS_PREFIX}/lib/libgpod.dylib, but the
# bundled copy was the pre-fix bytes from an earlier bundle pass. Every
# `Copy to Device` printed dozens of
#     pack_RGB_565: assertion 'dest_width < (gint)G_MAXUINT/2' failed
# and produced an `ArtworkDB` with no corresponding `.ithmb` thumbnail
# files. See `.ai/11-macos-dev-loop.md` §11.13 row 9.
bundle_collect_stale() {
  local bundle="$1"
  local -n _out="$2"
  _out=()
  local bundled base src bundled_hash src_hash
  while IFS= read -r -d '' bundled; do
    base="$(basename "${bundled}")"
    # Skip files that don't exist in /opt -- those were copied from
    # somewhere else (the build tree, Qt's own deploy, etc.) and we have
    # no canonical source to compare against.
    if [[ -f "${DEPS_PREFIX}/lib/${base}" ]]; then
      src="${DEPS_PREFIX}/lib/${base}"
    elif [[ -f "${DEPS_PREFIX}/lib/gstreamer-1.0/${base}" ]]; then
      src="${DEPS_PREFIX}/lib/gstreamer-1.0/${base}"
    else
      continue
    fi
    # Compare the __TEXT,__text section hash, NOT the whole file. The
    # whole-file hash always diverges between bundle and src because:
    #   * macdeployqt rewrote load commands (changes file header bytes),
    #   * codesign_bundle_adhoc appended a signature blob to __LINKEDIT,
    #   * install_name_tool may have grown the load-command pad region.
    # All of those touch file regions OUTSIDE __TEXT,__text. The compiled
    # code itself -- which is what determines whether the lib actually
    # contains the fixed pack_RGB_565 (or the broken one) -- lives in
    # __TEXT,__text and is untouched by any post-build cosmetic step. So
    # comparing __TEXT,__text gives a robust "does the bundled lib reflect
    # the current source?" answer.
    bundled_hash="$(otool -X -s __TEXT __text "${bundled}" 2>/dev/null | shasum -a 256 | awk '{print $1}')"
    src_hash="$(otool -X -s __TEXT __text "${src}" 2>/dev/null | shasum -a 256 | awk '{print $1}')"
    if [[ -n "${bundled_hash}" && -n "${src_hash}" && "${bundled_hash}" != "${src_hash}" ]]; then
      _out+=("${bundled}|${src}")
    fi
  done < <(find "${bundle}/Contents/Frameworks" "${bundle}/Contents/PlugIns" \
              \( -name "*.dylib" -o -name "*.so" \) -type f -print0 2>/dev/null)
}

# For each "bundled_path|src_path" entry in $2..$N, overwrite the bundled
# file with the fresh source bytes, then run bundle_repair_dirty on it so
# the just-copied /opt/... load commands get rewritten to @loader_path/...
# and the file is ad-hoc resigned. Idempotent.
bundle_repair_stale() {
  local bundle="$1"; shift
  local entry bundled src
  local -a refreshed=()
  for entry in "$@"; do
    bundled="${entry%%|*}"
    src="${entry#*|}"
    log "  refresh ${bundled##${bundle}/} <- ${src}"
    cp "${src}" "${bundled}" || { warn "  cp failed for ${bundled}"; continue; }
    refreshed+=("${bundled}")
  done
  if (( ${#refreshed[@]} > 0 )); then
    bundle_repair_dirty "${bundle}" "${refreshed[@]}"
  fi
}




codesign_bundle_adhoc() {
  local bundle="$1"
  [[ -d "${bundle}" ]] || fatal "codesign_bundle_adhoc: ${bundle} not a directory"

  info "Ad-hoc codesigning ${bundle} (this may take a moment)"

  # Sign every dylib first (deepest dependencies before things that link
  # them). We walk by mtime so newer files don't get processed before their
  # deps, but in practice dylibs in Frameworks/ are leaf nodes so any order
  # works after we --force.
  local -a signables=()
  while IFS= read -r -d '' f; do
    signables+=("${f}")
  done < <(find "${bundle}/Contents" \
              \( -name "*.dylib" -o -name "*.so" \) -type f -print0 2>/dev/null)

  # Some plugin scanners (gst-plugin-scanner, etc.) are mach-o executables
  # inside PlugIns/ — pick them up too.
  while IFS= read -r -d '' f; do
    # Skip already-listed dylibs/sos
    case "${f}" in *.dylib|*.so) continue ;; esac
    if file "${f}" 2>/dev/null | grep -q "Mach-O.*executable"; then
      signables+=("${f}")
    fi
  done < <(find "${bundle}/Contents/PlugIns" -type f -print0 2>/dev/null)

  if (( ${#signables[@]} > 0 )); then
    log "  Signing ${#signables[@]} embedded mach-o file(s)"
    codesign --force --sign - --timestamp=none "${signables[@]}" 2>&1 \
      | grep -v "^$" | tail -3 || true
  fi

  # Sign embedded .framework bundles
  if [[ -d "${bundle}/Contents/Frameworks" ]]; then
    local fw
    while IFS= read -r -d '' fw; do
      codesign --force --sign - --timestamp=none "${fw}" 2>/dev/null || \
        warn "codesign failed for ${fw}"
    done < <(find "${bundle}/Contents/Frameworks" -maxdepth 1 \
                -name "*.framework" -type d -print0 2>/dev/null)
  fi

  # Finally sign the bundle itself, deeply (to catch anything we missed).
  codesign --force --deep --sign - --timestamp=none "${bundle}" 2>&1 \
    | tail -3 || true

  # Verify
  if codesign --verify --deep --strict "${bundle}" 2>/dev/null; then
    ok "Ad-hoc signature verified: ${bundle}"
  else
    warn "codesign --verify reported issues — app should still launch on this Mac"
    warn "(only Gatekeeper/notarization-grade verification fails; ad-hoc is local-only)"
  fi
}

# Kill any running Strawberry so install/uninstall isn't blocked.
kill_running_app() {
  if pgrep -xq strawberry; then
    info "Quitting running ${APP_DISPLAY_NAME} instance"
    osascript -e "tell application \"${APP_DISPLAY_NAME}\" to quit" \
      >/dev/null 2>&1 || true
    sleep 1
    if pgrep -xq strawberry; then
      warn "Strawberry still running — sending SIGTERM"
      pkill -x strawberry || true
      sleep 1
    fi
    if pgrep -xq strawberry; then
      warn "Strawberry still running — sending SIGKILL"
      pkill -9 -x strawberry || true
    fi
  fi
}

cmd_install() {
  step "Installing ${APP_NAME} -> ${INSTALL_PREFIX}/${APP_NAME}"

  local src="${BUILD_DIR}/${APP_NAME}"
  local dest="${INSTALL_PREFIX}/${APP_NAME}"

  [[ -d "${src}" ]] \
    || fatal "${src} not found — build it first (./${SCRIPT_NAME} build && ./${SCRIPT_NAME} bundle)."

  # ----------------------------------------------------------------------
  # Dirty-bundle pre-flight check.
  #
  # Before: this gate only checked whether Contents/Frameworks/ existed at
  # all -- so an incrementally-built .app whose main binary had been
  # relinked (load commands still pointing at /opt/...) but whose
  # Frameworks/ dir was leftover from a previous bundle would silently
  # pass this check and ship to /Applications. The launching app would
  # then crash inside QGuiApplicationPrivate::createPlatformIntegration()
  # with the duplicate-QtCore signature documented at
  # `.ai/11-macos-dev-loop.md` §11.15. Multiple users -- including this
  # one, twice -- have lost an hour chasing source code that was innocent
  # by construction (the crash fires before main() returns).
  #
  # After: the gate runs the real §11.3 audit -- it scans every Mach-O
  # file in the .app for residual ${DEPS_PREFIX}/lib references and
  # auto-invokes `cmd_bundle` if any are found. If the bundle still comes
  # back dirty afterwards, we refuse to install. This makes the
  # "I forgot to bundle" failure mode impossible to reach via this
  # script.
  # ----------------------------------------------------------------------
  if (( DO_BUNDLE )); then
    if [[ ! -d "${src}/Contents/Frameworks" ]]; then
      warn "${src} has no Contents/Frameworks/ -- running bundle step."
      cmd_bundle
    else
      local -a preflight_dirty=() preflight_stale=()
      bundle_collect_dirty "${src}" preflight_dirty
      bundle_collect_stale "${src}" preflight_stale
      if (( ${#preflight_dirty[@]} > 0 || ${#preflight_stale[@]} > 0 )); then
        if (( ${#preflight_dirty[@]} > 0 )); then
          warn "${src} is DIRTY: ${#preflight_dirty[@]} file(s) still reference ${DEPS_PREFIX}/lib"
          local f
          for f in "${preflight_dirty[@]}"; do log "  DIRTY: ${f}"; done
        fi
        if (( ${#preflight_stale[@]} > 0 )); then
          warn "${src} is STALE: ${#preflight_stale[@]} bundled file(s) differ from ${DEPS_PREFIX}/lib source"
          local s
          for s in "${preflight_stale[@]}"; do log "  STALE: ${s%%|*}"; done
        fi
        info "Auto-running bundle step before install (this is the documented §11.1 safety net)"
        cmd_bundle

        # cmd_bundle has its own audit + repair for both dirt and
        # staleness, but defensively re-run both audits ourselves; if
        # either is still non-empty we refuse to install rather than
        # ship a broken .app.
        preflight_dirty=(); preflight_stale=()
        bundle_collect_dirty "${src}" preflight_dirty
        bundle_collect_stale "${src}" preflight_stale
        if (( ${#preflight_dirty[@]} > 0 || ${#preflight_stale[@]} > 0 )); then
          error "Bundle is still dirty/stale after auto-bundle:"
          for f in "${preflight_dirty[@]}"; do log "  DIRTY: ${f}"; done
          for s in "${preflight_stale[@]}"; do log "  STALE: ${s%%|*}"; done
          fatal "Refusing to install a bundle that would misbehave at runtime -- see §11.13 rows 1 and 9."
        fi
      fi
    fi
  fi

  kill_running_app

  local SUDO=""
  if [[ ! -w "${INSTALL_PREFIX}" ]] \
     || [[ -d "${dest}" && ! -w "${dest}" ]]; then
    SUDO="sudo"
    info "Elevated permissions required for ${INSTALL_PREFIX}"
  fi

  ${SUDO} mkdir -p "${INSTALL_PREFIX}"
  # rsync --delete: idempotent, removes stale files from previous installs.
  if have_cmd rsync; then
    ${SUDO} rsync -a --delete "${src}/" "${dest}/"
  else
    ${SUDO} rm -rf "${dest}"
    ${SUDO} cp -R "${src}" "${dest}"
  fi
  # Strip Apple's quarantine xattr so Gatekeeper doesn't block first launch.
  ${SUDO} xattr -dr com.apple.quarantine "${dest}" 2>/dev/null || true

  ok "Installed at ${dest}"
  log "Launch with:  open -a \"${APP_DISPLAY_NAME}\""
}

# User-data locations (cleaned only by `purge` or with explicit confirmation).
USER_DATA_PATHS=(
  "${HOME}/Library/Preferences/${APP_BUNDLE_ID}.plist"
  "${HOME}/Library/Preferences/Strawberry"
  "${HOME}/Library/Application Support/Strawberry"
  "${HOME}/Library/Caches/${APP_BUNDLE_ID}"
  "${HOME}/Library/Caches/Strawberry"
  "${HOME}/Library/Saved Application State/${APP_BUNDLE_ID}.savedState"
  "${HOME}/Library/HTTPStorages/${APP_BUNDLE_ID}"
  "${HOME}/Library/WebKit/${APP_BUNDLE_ID}"
)

cmd_uninstall() {
  step "Uninstalling ${APP_DISPLAY_NAME}"
  local dest="${INSTALL_PREFIX}/${APP_NAME}"

  kill_running_app

  if [[ -d "${dest}" ]]; then
    if confirm "Remove ${dest}?"; then
      local SUDO=""
      [[ -w "${INSTALL_PREFIX}" && -w "${dest}" ]] || SUDO="sudo"
      ${SUDO} rm -rf "${dest}"
      ok "Removed ${dest}"
    else
      warn "Skipped removing ${dest}"
    fi
  else
    ok "${dest} not present — nothing to uninstall"
  fi

  if confirm "Also remove user preferences and caches (Library/...)?"; then
    remove_user_data
  else
    log "User preferences preserved."
    log "  Use './${SCRIPT_NAME} purge' to remove them too."
  fi
}

remove_user_data() {
  local p
  for p in "${USER_DATA_PATHS[@]}"; do
    if [[ -e "${p}" ]]; then
      rm -rf "${p}"
      ok "Removed ${p}"
    fi
  done
  if have_cmd defaults; then
    defaults delete "${APP_BUNDLE_ID}" >/dev/null 2>&1 || true
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
  step "Purging everything (app + user data + build dir + cached tarball + deps tree)"
  ASSUME_YES=1 cmd_uninstall
  remove_user_data
  cmd_clean
  if [[ -d "${INSTALL_CACHE_DIR}" ]]; then
    rm -rf "${INSTALL_CACHE_DIR}"
    ok "Removed install cache (${INSTALL_CACHE_DIR})"
  fi
  if [[ -d "${DEPS_PREFIX}" ]]; then
    if confirm "Remove ${DEPS_PREFIX} (the upstream deps tree, ~500 MB)?"; then
      sudo rm -rf "${DEPS_PREFIX}"
      ok "Removed ${DEPS_PREFIX}"
    else
      log "Kept ${DEPS_PREFIX}; pass -y to remove non-interactively."
    fi
  fi
  ok "System restored to a clean state"
}

cmd_status() {
  step "Status"
  require_macos
  detect_macos_version
  detect_arch
  if have_cmd brew; then
    BREW_PREFIX="$(brew --prefix)"
  fi

  printf "  %-30s %s\n" "Repo root:"        "${REPO_ROOT}"
  printf "  %-30s %s\n" "Build dir:"        "${BUILD_DIR}"
  printf "  %-30s %s\n" "Install prefix:"   "${INSTALL_PREFIX}"
  printf "  %-30s %s\n" "Build type:"       "${BUILD_TYPE}"
  printf "  %-30s %s\n" "Parallel jobs:"    "${JOBS}"
  printf "  %-30s %s\n" "Architecture:"     "${ARCH}"
  printf "  %-30s %s\n" "Deps tarball:"     "${DEPS_TARBALL_NAME}"
  printf "  %-30s %s\n" "Deps source repo:" "${DEPS_REPO}"
  printf "  %-30s %s\n" "Deps install prefix:" "${DEPS_PREFIX}"
  printf "  %-30s %s\n" "Homebrew prefix:"  "${BREW_PREFIX:-<not installed>}"
  echo
  if deps_installed; then
    ok "Deps tarball extracted: ${DEPS_PREFIX}"
  else
    warn "Deps tarball NOT extracted (run './${SCRIPT_NAME} deps')"
  fi
  if [[ -f "$(deps_tarball_cache_path)" ]]; then
    local sz
    sz="$(du -h "$(deps_tarball_cache_path)" 2>/dev/null | awk '{print $1}')"
    ok "Tarball cached locally: $(deps_tarball_cache_path) (${sz})"
  fi
  if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    ok "Build dir configured"
  else
    warn "Build dir not configured"
  fi
  if [[ -d "${BUILD_DIR}/${APP_NAME}" ]]; then
    ok "Built bundle present: ${BUILD_DIR}/${APP_NAME}"
    if [[ -d "${BUILD_DIR}/${APP_NAME}/Contents/Frameworks" ]]; then
      ok "Bundle has been deployed (Contents/Frameworks present)"
    else
      warn "Bundle not yet deployed (no Contents/Frameworks)"
    fi
  else
    warn "No built bundle in build dir"
  fi
  if [[ -d "${INSTALL_PREFIX}/${APP_NAME}" ]]; then
    ok "Installed: ${INSTALL_PREFIX}/${APP_NAME}"
  else
    warn "Not installed in ${INSTALL_PREFIX}"
  fi
}

cmd_doctor() {
  step "Environment doctor"
  require_macos
  detect_macos_version
  detect_arch
  ensure_brew

  local issues=0
  check_cmd() {
    if have_cmd "$1"; then
      ok "$1 -> $(command -v "$1")"
    else
      error "$1 not in PATH"
      issues=$((issues + 1))
    fi
  }

  check_cmd cmake
  check_cmd pkg-config
  check_cmd git
  check_cmd curl
  check_cmd tar
  check_cmd sudo
  check_cmd rsync

  for pkg in "${BREW_PKGS_BUILDTOOLS[@]}"; do
    if brew_installed "${pkg}"; then
      ok "brew formula ${pkg}: $(brew list --versions "${pkg}" | head -1)"
    else
      error "brew formula ${pkg}: missing"
      issues=$((issues + 1))
    fi
  done

  if deps_installed; then
    ok "Strawberry macOS deps: ready at ${DEPS_PREFIX}"
  else
    warn "Strawberry macOS deps not yet downloaded (run './${SCRIPT_NAME} deps')"
  fi

  if (( issues > 0 )); then
    error "${issues} issue(s) detected — run './${SCRIPT_NAME} deps' to fix."
    exit 1
  fi
  ok "Environment looks healthy."
}

cmd_all() {
  cmd_deps
  cmd_build
  if (( DO_BUNDLE )); then
    cmd_bundle
  else
    warn "Skipping bundle step (--no-bundle); installed app will need ${DEPS_PREFIX} at runtime."
  fi
  cmd_install
  step "Done"
  log "Open the app with:  open -a \"${APP_DISPLAY_NAME}\""
  log "Reset to clean state:  ./${SCRIPT_NAME} purge -y"
}

cmd_help() {
  sed -n '2,55p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# ------------------------------------------------------------------------------
# Argument parsing
# ------------------------------------------------------------------------------
parse_args() {
  COMMAND=""
  while (( $# > 0 )); do
    case "$1" in
      deps|configure|build|bundle|install|uninstall|purge|clean|status|doctor|all|help)
        if [[ -z "${COMMAND}" ]]; then
          COMMAND="$1"
        else
          fatal "Multiple commands given: '${COMMAND}' and '$1'."
        fi
        shift
        ;;
      --build-dir)    BUILD_DIR="$2";       shift 2 ;;
      --build-dir=*)  BUILD_DIR="${1#*=}";  shift   ;;
      --prefix)       INSTALL_PREFIX="$2";  shift 2 ;;
      --prefix=*)     INSTALL_PREFIX="${1#*=}"; shift ;;
      --build-type)   BUILD_TYPE="$2";      shift 2 ;;
      --build-type=*) BUILD_TYPE="${1#*=}"; shift   ;;
      --jobs|-j)      JOBS="$2";            shift 2 ;;
      --jobs=*)       JOBS="${1#*=}";       shift   ;;
      --no-bundle)    DO_BUNDLE=0;          shift   ;;
      --refresh-deps) REFRESH_DEPS=1;       shift   ;;
      --yes|-y)       ASSUME_YES=1;         shift   ;;
      --verbose|-v)   VERBOSE=1;            shift   ;;
      -h|--help)      COMMAND="help";       shift   ;;
      --)             shift; break ;;
      -*)             fatal "Unknown option: $1 (try --help)" ;;
      *)              fatal "Unexpected argument: $1 (try --help)" ;;
    esac
  done
  COMMAND="${COMMAND:-all}"
  (( VERBOSE )) && set -x
  case "${BUILD_DIR}" in
    /*) ;;
    *)  BUILD_DIR="${REPO_ROOT}/${BUILD_DIR#./}" ;;
  esac
}

main() {
  require_macos
  detect_arch
  parse_args "$@"
  case "${COMMAND}" in
    deps)      cmd_deps ;;
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
    help)      cmd_help ;;
    *)         fatal "Unknown command: ${COMMAND}" ;;
  esac
}

main "$@"