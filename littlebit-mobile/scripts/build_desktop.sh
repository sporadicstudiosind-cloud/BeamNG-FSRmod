#!/usr/bin/env bash
# Build patched llama.cpp for this machine (Linux/macOS). Extra cmake flags are passed through,
# e.g. ./build_desktop.sh -DGGML_CUDA=ON   or   ./build_desktop.sh -DGGML_VULKAN=ON
set -euo pipefail
source "$(dirname "$0")/common.sh"

DIR="${LLAMA_CPP_DIR:-$LB_ROOT/build/llama.cpp}"
get_patched_llama_cpp "$DIR"

cmake -S "$DIR" -B "$DIR/build" -DCMAKE_BUILD_TYPE=Release -DLLAMA_BUILD_TESTS=OFF "$@"
cmake --build "$DIR/build" --config Release -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" \
    --target llama-cli llama-completion llama-server llama-perplexity llama-bench llama-quantize

echo
echo "binaries: $DIR/build/bin"
echo "convert:  python $LB_ROOT/convert_littlebit_to_gguf.py <checkpoint> --llama-cpp $DIR --outfile model.gguf"
