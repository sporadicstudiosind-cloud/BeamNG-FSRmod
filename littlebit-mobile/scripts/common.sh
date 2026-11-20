# shared settings for the build scripts (sourced, not executed)

# llama.cpp release the patch was tested against. b12257 is also the version Ollama pins (LLAMA_CPP_VERSION).
: "${LLAMA_CPP_REF:=b12257}"
: "${LLAMA_CPP_REPO:=https://github.com/ggml-org/llama.cpp.git}"

LB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LB_PATCH="$LB_ROOT/patches/0001-llama-littlebit-factorized-linear.patch"

# get_patched_llama_cpp <dir>: clone llama.cpp at $LLAMA_CPP_REF into <dir> (if missing) and apply the LittleBit patch
get_patched_llama_cpp() {
    local dir="$1"
    if [ ! -d "$dir/.git" ]; then
        git clone --depth 1 --branch "$LLAMA_CPP_REF" "$LLAMA_CPP_REPO" "$dir"
    fi
    if git -C "$dir" apply --reverse --check "$LB_PATCH" 2>/dev/null; then
        echo "LittleBit patch already applied in $dir"
    else
        git -C "$dir" apply "$LB_PATCH"
        echo "applied LittleBit patch to $dir"
    fi
}
