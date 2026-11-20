#!/data/data/com.termux/files/usr/bin/bash
# Build patched llama.cpp directly on the phone inside Termux (no PC, no root).
#   pkg install git && git clone <this repo> && cd <repo>/littlebit-mobile && bash scripts/termux_build.sh
set -euo pipefail

pkg update -y
pkg install -y git cmake clang make libandroid-spawn

source "$(dirname "$0")/common.sh"
DIR="${LLAMA_CPP_DIR:-$HOME/llama.cpp-littlebit}"
get_patched_llama_cpp "$DIR"

cmake -S "$DIR" -B "$DIR/build" -DCMAKE_BUILD_TYPE=Release -DGGML_OPENMP=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_TESTS=OFF
# keep -j low: phones throttle and Termux gets killed under memory pressure
cmake --build "$DIR/build" --config Release -j"${JOBS:-4}" --target llama-cli llama-server llama-bench

echo
echo "done. run:  $DIR/build/bin/llama-cli -m ~/model.gguf -c 2048 -t 4"
echo "or serve:   $DIR/build/bin/llama-server -m ~/model.gguf -c 2048 --port 8080   (then open http://127.0.0.1:8080)"
