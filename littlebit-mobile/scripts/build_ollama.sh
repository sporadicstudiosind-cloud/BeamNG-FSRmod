#!/usr/bin/env bash
# Build Ollama from source with LittleBit support.
#
# Ollama (0.30+) runs llama.cpp's llama-server for GGUF models and applies every *.patch in
# llama/compat/ (in filename order) to the llama.cpp version pinned in LLAMA_CPP_VERSION.
# We add our patch there as 900-littlebit-factorized-linear.patch, so it lands after Ollama's own hooks.
#
# Needs: Go (version from ollama/go.mod), CMake >= 3.24, a C/C++ compiler.
# Extra args go to Ollama's cmake configure, e.g. -DOLLAMA_LLAMA_BACKENDS=vulkan
set -euo pipefail
source "$(dirname "$0")/common.sh"

: "${OLLAMA_REPO:=https://github.com/ollama/ollama.git}"
: "${OLLAMA_REF:=main}"
DIR="${OLLAMA_DIR:-$LB_ROOT/build/ollama}"

if [ ! -d "$DIR/.git" ]; then
    git clone --depth 1 --branch "$OLLAMA_REF" "$OLLAMA_REPO" "$DIR"
fi

pinned="$(cat "$DIR/LLAMA_CPP_VERSION")"
if [ "$pinned" != "$LLAMA_CPP_REF" ]; then
    echo "warning: this Ollama pins llama.cpp $pinned, the patch was tested on $LLAMA_CPP_REF; it usually still applies" >&2
fi

cp "$LB_PATCH" "$DIR/llama/compat/900-littlebit-factorized-linear.patch"

cd "$DIR"
cmake -B build . "$@"
cmake --build build --parallel "$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
go build -o ollama .

cat <<EOF

built $DIR/ollama
  OLLAMA_LIBRARY_PATH=$DIR/build/lib/ollama $DIR/ollama serve &      # (or: cd $DIR && go run . serve)
  $DIR/ollama create littlebit -f $LB_ROOT/ollama/Modelfile
  $DIR/ollama run littlebit
EOF
