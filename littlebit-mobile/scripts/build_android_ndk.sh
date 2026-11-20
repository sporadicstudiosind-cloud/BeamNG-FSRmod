#!/usr/bin/env bash
# Cross-compile patched llama.cpp for Android arm64 with the NDK (run on a PC).
#   export ANDROID_NDK=/path/to/android-ndk-r27c
#   ./build_android_ndk.sh                 # then push with adb, see README
set -euo pipefail
source "$(dirname "$0")/common.sh"

: "${ANDROID_NDK:?set ANDROID_NDK to your NDK root (e.g. ~/Android/Sdk/ndk/27.2.12479018)}"
: "${ANDROID_ABI:=arm64-v8a}"
: "${ANDROID_PLATFORM:=android-28}"

DIR="${LLAMA_CPP_DIR:-$LB_ROOT/build/llama.cpp}"
OUT="${OUT_DIR:-$LB_ROOT/build/android-$ANDROID_ABI}"
get_patched_llama_cpp "$DIR"

# flags from llama.cpp docs/android.md; the CPU backend picks NEON/dotprod/i8mm/SVE/SME kernels at runtime
cmake -S "$DIR" -B "$DIR/build-android" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_NATIVE=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_LLAMAFILE=OFF \
    -DLLAMA_OPENSSL=OFF \
    -DLLAMA_BUILD_TESTS=OFF \
    "$@"
cmake --build "$DIR/build-android" --config Release -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" \
    --target llama-cli llama-completion llama-server llama-bench
cmake --install "$DIR/build-android" --prefix "$OUT" --config Release

cat <<EOF

installed to $OUT
push and run:
  adb shell mkdir -p /data/local/tmp/lb
  adb push $OUT/. /data/local/tmp/lb/
  adb push model.gguf /data/local/tmp/lb/
  adb shell 'cd /data/local/tmp/lb && LD_LIBRARY_PATH=lib ./bin/llama-cli -m model.gguf -c 2048 -t 4'
EOF
