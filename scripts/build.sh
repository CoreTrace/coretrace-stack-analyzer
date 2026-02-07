#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="build"
BUILD_TYPE="Release"
GENERATOR=""
JOBS=""
CLEAN=0
CONFIGURE_ONLY=0

LLVM_DIR_ENV="${LLVM_DIR:-}"
CLANG_DIR_ENV="${Clang_DIR:-}"
LLVM_DIR="${LLVM_DIR_ENV}"
Clang_DIR="${CLANG_DIR_ENV}"
LLVM_DIR_EXPLICIT=0
CLANG_DIR_EXPLICIT=0

if [ -n "$LLVM_DIR" ]; then
  LLVM_DIR_EXPLICIT=1
fi
if [ -n "$Clang_DIR" ]; then
  CLANG_DIR_EXPLICIT=1
fi

usage() {
  cat <<'USAGE'
Usage: build.sh [options]

Options:
  --build-dir <dir>                 Build directory (default: build)
  --type <Release|Debug|RelWithDebInfo>
                                   Build type (default: Release)
  --generator <Ninja|Unix Makefiles>
                                   CMake generator (default: Ninja if available)
  --llvm-dir <path>                 LLVM CMake directory (or set LLVM_DIR)
  --clang-dir <path>                Clang CMake directory (or set Clang_DIR)
  --jobs <n>                        Parallel build jobs
  --clean                           Delete build directory before configuring
  --configure-only                  Only run CMake configure step
  -h, --help                        Show this help

Examples:
  ./build.sh --type Release
  ./build.sh --clean --build-dir out/build
  LLVM_DIR=/opt/llvm/lib/cmake/llvm Clang_DIR=/opt/llvm/lib/cmake/clang ./build.sh
USAGE
}

die() {
  echo "error: $*" >&2
  exit 1
}

note() {
  echo "==> $*"
}

require_arg() {
  local flag="$1"
  local value="${2:-}"
  if [ -z "$value" ]; then
    die "Missing value for $flag"
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir)
      require_arg "$1" "${2:-}"
      BUILD_DIR="$2"
      shift 2
      ;;
    --type)
      require_arg "$1" "${2:-}"
      BUILD_TYPE="$2"
      shift 2
      ;;
    --generator)
      require_arg "$1" "${2:-}"
      GENERATOR="$2"
      shift 2
      ;;
    --llvm-dir)
      require_arg "$1" "${2:-}"
      LLVM_DIR="$2"
      LLVM_DIR_EXPLICIT=1
      shift 2
      ;;
    --clang-dir)
      require_arg "$1" "${2:-}"
      Clang_DIR="$2"
      CLANG_DIR_EXPLICIT=1
      shift 2
      ;;
    --jobs)
      require_arg "$1" "${2:-}"
      JOBS="$2"
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    --configure-only)
      CONFIGURE_ONLY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
done

case "$BUILD_TYPE" in
  Release|Debug|RelWithDebInfo)
    ;;
  *)
    die "Invalid build type: $BUILD_TYPE"
    ;;
esac

if [ -z "$BUILD_DIR" ]; then
  die "Build directory cannot be empty"
fi

if [ "$BUILD_DIR" != /* ]; then
  BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi

if [ "$CLEAN" -eq 1 ]; then
  if [ "$BUILD_DIR" = "/" ] || [ "$BUILD_DIR" = "$ROOT_DIR" ]; then
    die "Refusing to clean build directory: $BUILD_DIR"
  fi
  note "Cleaning build directory: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

if ! command -v cmake >/dev/null 2>&1; then
  die "cmake not found in PATH"
fi

OS_NAME="$(uname -s)"

if [ -z "$GENERATOR" ]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  else
    GENERATOR="Unix Makefiles"
  fi
fi

if [ "$GENERATOR" = "Ninja" ] && ! command -v ninja >/dev/null 2>&1; then
  die "Generator 'Ninja' requested but ninja is not installed"
fi

if [ -n "$JOBS" ]; then
  if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [ "$JOBS" -lt 1 ]; then
    die "Invalid jobs count: $JOBS"
  fi
else
  case "$OS_NAME" in
    Darwin)
      if command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
      fi
      ;;
    Linux)
      if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc 2>/dev/null || true)"
      fi
      ;;
  esac
  if [ -z "${JOBS:-}" ] && command -v getconf >/dev/null 2>&1; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
  fi
  if ! [[ "${JOBS:-}" =~ ^[0-9]+$ ]] || [ "${JOBS:-0}" -lt 1 ]; then
    JOBS=1
  fi
fi

normalize_dirs() {
  if [ -n "$LLVM_DIR" ] && [ -z "$Clang_DIR" ]; then
    local candidate
    candidate="$(dirname "$LLVM_DIR")/clang"
    if [ -d "$candidate" ]; then
      Clang_DIR="$candidate"
    fi
  fi

  if [ -n "$Clang_DIR" ] && [ -z "$LLVM_DIR" ]; then
    local candidate
    candidate="$(dirname "$Clang_DIR")/llvm"
    if [ -d "$candidate" ]; then
      LLVM_DIR="$candidate"
    fi
  fi
}

validate_explicit_dirs() {
  if [ "$LLVM_DIR_EXPLICIT" -eq 1 ] && [ -n "$LLVM_DIR" ] && [ ! -d "$LLVM_DIR" ]; then
    die "LLVM_DIR does not exist: $LLVM_DIR"
  fi
  if [ "$CLANG_DIR_EXPLICIT" -eq 1 ] && [ -n "$Clang_DIR" ] && [ ! -d "$Clang_DIR" ]; then
    die "Clang_DIR does not exist: $Clang_DIR"
  fi
}

detect_from_brew() {
  [ "$OS_NAME" = "Darwin" ] || return 0
  command -v brew >/dev/null 2>&1 || return 0

  local formula
  local prefix
  for formula in llvm llvm@20 llvm@19 llvm@18; do
    prefix="$(brew --prefix "$formula" 2>/dev/null || true)"
    if [ -n "$prefix" ] && [ -d "$prefix" ]; then
      if [ -z "$LLVM_DIR" ] && [ -d "$prefix/lib/cmake/llvm" ]; then
        LLVM_DIR="$prefix/lib/cmake/llvm"
      fi
      if [ -z "$Clang_DIR" ] && [ -d "$prefix/lib/cmake/clang" ]; then
        Clang_DIR="$prefix/lib/cmake/clang"
      fi
      if [ -n "$LLVM_DIR" ] && [ -n "$Clang_DIR" ]; then
        return 0
      fi
    fi
  done
}

detect_from_llvm_config() {
  command -v llvm-config >/dev/null 2>&1 || return 0

  local cmake_dir
  cmake_dir="$(llvm-config --cmakedir 2>/dev/null || true)"
  if [ -n "$cmake_dir" ] && [ -d "$cmake_dir" ]; then
    if [ -z "$LLVM_DIR" ]; then
      LLVM_DIR="$cmake_dir"
    fi
    if [ -z "$Clang_DIR" ]; then
      local clang_dir
      clang_dir="$(dirname "$cmake_dir")/clang"
      if [ -d "$clang_dir" ]; then
        Clang_DIR="$clang_dir"
      fi
    fi
  fi
}

detect_from_prefixes() {
  local prefixes=()
  shopt -s nullglob
  prefixes+=(/usr/lib/llvm-*)
  prefixes+=(/usr/lib64/llvm-*)
  prefixes+=(/usr/lib/llvm)
  prefixes+=(/usr/local/llvm*)
  prefixes+=(/opt/llvm*)
  shopt -u nullglob

  local prefix
  for prefix in "${prefixes[@]}"; do
    [ -d "$prefix" ] || continue
    if [ -z "$LLVM_DIR" ] && [ -d "$prefix/lib/cmake/llvm" ]; then
      LLVM_DIR="$prefix/lib/cmake/llvm"
    fi
    if [ -z "$Clang_DIR" ] && [ -d "$prefix/lib/cmake/clang" ]; then
      Clang_DIR="$prefix/lib/cmake/clang"
    fi
    if [ -n "$LLVM_DIR" ] && [ -n "$Clang_DIR" ]; then
      return 0
    fi
  done
}

normalize_dirs
validate_explicit_dirs

if [ "$LLVM_DIR_EXPLICIT" -eq 0 ] && [ "$CLANG_DIR_EXPLICIT" -eq 0 ]; then
  if [ -z "$LLVM_DIR" ] || [ -z "$Clang_DIR" ]; then
    detect_from_brew
    detect_from_llvm_config
    detect_from_prefixes
    normalize_dirs
  fi
fi

if [ -z "$LLVM_DIR" ]; then
  die "LLVM_DIR not found. Set LLVM_DIR or pass --llvm-dir (e.g., LLVM_DIR=/opt/llvm/lib/cmake/llvm)."
fi
if [ -z "$Clang_DIR" ]; then
  die "Clang_DIR not found. Set Clang_DIR or pass --clang-dir (e.g., Clang_DIR=/opt/llvm/lib/cmake/clang)."
fi
if [ ! -d "$LLVM_DIR" ]; then
  die "LLVM_DIR does not exist: $LLVM_DIR"
fi
if [ ! -d "$Clang_DIR" ]; then
  die "Clang_DIR does not exist: $Clang_DIR"
fi

note "Configuring"
note "  Root:       $ROOT_DIR"
note "  Build dir:  $BUILD_DIR"
note "  Type:       $BUILD_TYPE"
note "  Generator:  $GENERATOR"
note "  Jobs:       $JOBS"
note "  LLVM_DIR:   $LLVM_DIR"
note "  Clang_DIR:  $Clang_DIR"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -G "$GENERATOR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DLLVM_DIR="$LLVM_DIR" \
  -DClang_DIR="$Clang_DIR"

if [ "$CONFIGURE_ONLY" -eq 1 ]; then
  note "Configure-only requested; skipping build"
  exit 0
fi

note "Building"
cmake --build "$BUILD_DIR" -j "$JOBS"
