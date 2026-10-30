#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<'USAGE'
Usage: ./release.sh

Builds the optimized Release for the current platform, runs the full checks,
stages the SDK with build provenance, and creates a ZIP (Windows) or tar.gz
(Linux) archive under build/release-artifacts/.

Set LLVM_ROOT to the LLVM 23 installation prefix if it is not auto-detected.
USAGE
  exit 0
fi
if [[ $# -ne 0 ]]; then
  echo "release.sh does not accept build-mode arguments; use build.sh for checks." >&2
  exit 2
fi

platform="$(uname -s)"
case "$platform" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) platform=windows; preset=windows-clang-cl-release ;;
  Linux*) platform=linux; preset=linux-clang-release ;;
  *) echo "Unsupported release platform: $platform" >&2; exit 2 ;;
esac

if [[ -z "${LLVM_ROOT:-}" ]]; then
  if [[ "$platform" == windows ]]; then
    clang_cl="$(command -v clang-cl.exe || command -v clang-cl || true)"
    if [[ -z "$clang_cl" ]]; then
      echo "Set LLVM_ROOT or put clang-cl.exe on PATH." >&2
      exit 2
    fi
    LLVM_ROOT="$(cd "$(dirname "$clang_cl")/.." && pwd)"
    if command -v cygpath >/dev/null 2>&1; then LLVM_ROOT="$(cygpath -m "$LLVM_ROOT")"; fi
  elif [[ -x /opt/llvm-23/bin/clang ]]; then
    LLVM_ROOT=/opt/llvm-23
  elif [[ -x /usr/lib/llvm-23/bin/clang ]]; then
    LLVM_ROOT=/usr/lib/llvm-23
  else
    echo "Set LLVM_ROOT to the LLVM 23 prefix (for example /opt/llvm-23)." >&2
    exit 2
  fi
fi
export LLVM_ROOT

case "$platform" in
  windows)
    build_dir="$ROOT_DIR/build/windows/clang-cl/release"
    target=x86_64-pc-windows-msvc
    clang="$LLVM_ROOT/bin/clang-cl.exe"
    lld="$LLVM_ROOT/bin/lld-link.exe"
    archive_base="$ROOT_DIR/build/release-artifacts/ombasic-windows-sdk"
    archive_format=zip
    ;;
  linux)
    build_dir="$ROOT_DIR/build/linux/clang/release"
    target=x86_64-unknown-linux-gnu
    clang="$LLVM_ROOT/bin/clang"
    lld="$LLVM_ROOT/bin/ld.lld"
    archive_base="$ROOT_DIR/build/release-artifacts/ombasic-linux-sdk"
    archive_format=gztar
    ;;
esac

python_cmd="${PYTHON:-}"
if [[ -z "$python_cmd" ]]; then python_cmd="$(command -v python3 || command -v python || true)"; fi
if [[ -z "$python_cmd" ]]; then echo "Python 3 is required." >&2; exit 2; fi

cmake --preset "$preset"
cmake --build --preset "$preset" --parallel
"$python_cmd" tools/architecture_check/check.py --root .
"$python_cmd" tools/spec_check/check.py --root .
ctest --preset "$preset"

if [[ "$platform" == windows ]]; then
  executable="$build_dir/ombasic.exe"
else
  executable="$build_dir/ombasic"
fi
"$executable" check examples/hello/main.bas

python_build_dir="$build_dir"
python_root_dir="$ROOT_DIR"
package_dir="$ROOT_DIR/build/package/$platform"
python_package_dir="$package_dir"
python_archive_base="$archive_base"
python_clang="$clang"
python_lld="$lld"
if [[ "$platform" == windows ]] && command -v cygpath >/dev/null 2>&1; then
  python_build_dir="$(cygpath -m "$build_dir")"
  python_root_dir="$(cygpath -m "$ROOT_DIR")"
  python_package_dir="$(cygpath -m "$package_dir")"
  python_archive_base="$(cygpath -m "$archive_base")"
  python_clang="$(cygpath -m "$clang")"
  python_lld="$(cygpath -m "$lld")"
fi

"$python_cmd" tests/tooling/install_sdk_without_build_tree_test.py \
  cmake "$python_build_dir" "$python_root_dir" Release
cmake --install "$build_dir" --prefix "$package_dir"
llvm_config="$LLVM_ROOT/bin/llvm-config"
if [[ "$platform" == windows ]]; then llvm_config="$llvm_config.exe"; fi
"$python_cmd" tools/release/write_provenance.py \
  --root "$python_root_dir" \
  --output "$python_package_dir/share/ombasic/release-provenance.json" \
  --target "$target" \
  --llvm-version "$("$llvm_config" --version)" \
  --clang "$python_clang" \
  --lld "$python_lld"

"$python_cmd" - "$python_package_dir" "$python_archive_base" "$archive_format" <<'PY'
import shutil
import sys
shutil.make_archive(sys.argv[2], sys.argv[3], root_dir=sys.argv[1])
PY

echo "Release SDK created: ${archive_base}.$([[ "$archive_format" == zip ]] && echo zip || echo tar.gz)"
