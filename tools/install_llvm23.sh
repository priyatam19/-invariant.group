#!/usr/bin/env bash
# Official stable Linux x86-64 development toolchain, installed locally.
set -euo pipefail
if [[ "$#" != 1 ]]; then
  echo "Usage: $0 NEW_INSTALL_DIRECTORY" >&2
  exit 1
fi
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  echo "This pinned archive supports Linux x86-64 only." >&2
  exit 1
fi
if [[ -e "$1" ]]; then
  echo "Install directory already exists: $1" >&2
  exit 1
fi
archive_dir="$(mktemp -d)"
trap 'rm -rf "$archive_dir"' EXIT
archive="$archive_dir/LLVM-23.1.1-Linux-X64.tar.zst"
curl --fail --location --retry 3 --output "$archive" \
  https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.1/LLVM-23.1.1-Linux-X64.tar.zst
expected=b7ddbabd70fa1d206948bc83f59e59aa84eaf4cd09b6b89cc3ece28177710a6f
actual="$(sha256sum "$archive")"
if [[ "${actual%% *}" != "$expected" ]]; then
  echo "LLVM archive checksum mismatch" >&2
  exit 1
fi
mkdir -p "$1"
tar --zstd -xf "$archive" --strip-components=1 -C "$1"
"$1/bin/llvm-config" --version
