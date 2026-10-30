#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'USAGE'
Usage: ./build.sh [release|asan-ubsan|tsan|fuzz]

  release      Platform-native optimized build, full CTest, SDK relocation check, package
  asan-ubsan   Linux Clang RelWithDebInfo build and full CTest
  tsan         Linux Clang RelWithDebInfo build and full CTest
  fuzz         Linux Clang ASan/UBSan fuzzer build and short smoke run

With no argument, builds release for the current platform.
Set LLVM_ROOT to the LLVM 23 installation prefix when it is not auto-detected.
USAGE
}

mode="${1:-release}"
case "$mode" in
  -h|--help) usage; exit 0 ;;
  release|asan-ubsan|tsan|fuzz) ;;
  *) echo "Unknown build mode: $mode" >&2; usage >&2; exit 2 ;;
esac

platform="$(uname -s)"
case "$platform" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*)
    platform="windows"
    if [[ "$mode" != "release" ]]; then
      echo "Mode '$mode' is available only on Linux; Windows uses the Release preset." >&2
      exit 2
    fi
    preset="windows-clang-cl-release"
    build_dir="$ROOT_DIR/build/windows/clang-cl/release"
    if [[ -z "${LLVM_ROOT:-}" ]]; then
      clang_cl="$(command -v clang-cl.exe || command -v clang-cl || true)"
      if [[ -z "$clang_cl" ]]; then
        echo "Set LLVM_ROOT or put clang-cl.exe on PATH." >&2
        exit 2
      fi
      LLVM_ROOT="$(cd "$(dirname "$clang_cl")/.." && pwd)"
      if command -v cygpath >/dev/null 2>&1; then
        LLVM_ROOT="$(cygpath -m "$LLVM_ROOT")"
      fi
    fi
    ;;
  Linux*)
    platform="linux"
    if [[ -z "${LLVM_ROOT:-}" ]]; then
      if [[ -x /opt/llvm-23/bin/clang ]]; then
        LLVM_ROOT=/opt/llvm-23
      elif [[ -x /usr/lib/llvm-23/bin/clang ]]; then
        LLVM_ROOT=/usr/lib/llvm-23
      else
        echo "Set LLVM_ROOT to the LLVM 23 prefix (for example /opt/llvm-23)." >&2
        exit 2
      fi
    fi
    case "$mode" in
      release) preset="linux-clang-release"; build_dir="$ROOT_DIR/build/linux/clang/release" ;;
      asan-ubsan) preset="linux-clang-asan-ubsan"; build_dir="$ROOT_DIR/build/linux/clang/asan-ubsan" ;;
      tsan) preset="linux-clang-tsan"; build_dir="$ROOT_DIR/build/linux/clang/tsan" ;;
      fuzz) preset="linux-clang-fuzz"; build_dir="$ROOT_DIR/build/linux/clang/fuzz" ;;
    esac
    ;;
  *)
    echo "Unsupported host platform: $platform" >&2
    exit 2
    ;;
esac

export LLVM_ROOT
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel

if [[ "$mode" == "release" ]]; then
  python_cmd="${PYTHON:-}"
  if [[ -z "$python_cmd" ]]; then
    python_cmd="$(command -v python3 || command -v python || true)"
  fi
  if [[ -z "$python_cmd" ]]; then
    echo "Python 3 is required for project checks." >&2
    exit 2
  fi
  "$python_cmd" tools/architecture_check/check.py --root .
  "$python_cmd" tools/spec_check/check.py --root .
  ctest --preset "$preset"
  python_build_dir="$build_dir"
  python_root_dir="$ROOT_DIR"
  if [[ "$platform" == "windows" ]] && command -v cygpath >/dev/null 2>&1; then
    python_build_dir="$(cygpath -m "$build_dir")"
    python_root_dir="$(cygpath -m "$ROOT_DIR")"
  fi
  "$python_cmd" tests/tooling/install_sdk_without_build_tree_test.py \
    cmake "$python_build_dir" "$python_root_dir" Release
  package_dir="$ROOT_DIR/build/package/$platform"
  cmake --install "$build_dir" --prefix "$package_dir"
  tar -czf "$ROOT_DIR/build/ombasic-sdk-$platform.tar.gz" -C "$package_dir" .
  echo "SDK package: $ROOT_DIR/build/ombasic-sdk-$platform.tar.gz"
elif [[ "$mode" == "asan-ubsan" ]]; then
  ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:strict_string_checks=1}" \
    UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" \
    ctest --preset "$preset"
elif [[ "$mode" == "tsan" ]]; then
  TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}" \
    ctest --preset "$preset"
else
  ctest --preset "$preset"
  for fuzzer in \
    ombasic_fuzz_syntax ombasic_fuzz_metadata ombasic_fuzz_import_path \
    ombasic_fuzz_distribution ombasic_fuzz_hir_verifier \
    ombasic_fuzz_mir_verifier ombasic_fuzz_distribution_serializer \
    ombasic_fuzz_native_descriptor_codec ombasic_fuzz_json_parser; do
    "$build_dir/$fuzzer" -runs=200 -max_len=4096
  done
  "$build_dir/ombasic_fuzz_llvm_backend" -runs=20 -max_len=4096
fi
