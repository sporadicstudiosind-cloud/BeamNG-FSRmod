# LittleBit-2 → llama.cpp / Ollama (sub-1-bit LLMs on phones)

This folder lets llama.cpp (and an Ollama built from source) run models compressed with
**LittleBit / LittleBit-2**, Samsung's sub-1-bit LLM compression
([NeurIPS 2025](https://arxiv.org/abs/2506.13771), [ICML 2026](https://arxiv.org/abs/2603.00042),
[code](https://github.com/SamsungLabs/LittleBit)). Stock llama.cpp and Ollama can't load these
models: LittleBit swaps every weight matrix for a different layer type.

> It is unrelated to the BeamNG FSR mod in the rest of this repository and only shares the repo.

## What LittleBit-2 actually is (and isn't)

- It is **not an optimizer** you run on a model in a few minutes. It is a compression *format* plus a
  **quantization-aware training (QAT)** recipe. Each linear layer becomes
  `W ≈ diag(u1) · sign(U) · diag(u2·v1) · sign(V) · diag(v2)` (plus an optional residual branch of the
  same form), with `U` and `V` holding only ±1. The model is then distilled from the original for
  epochs on a GPU (the authors use 5 epochs; the community 8B below got 0.5 epochs on one H200).
- "LittleBit-2" changes only the **initialization** (Joint-ITQ rotation of the latent factors,
  `--use_itq True`). The deployed layer is identical, so one runtime serves both.
- At 0.55 bits/weight quality drops a lot today; see the numbers below.

## What's here

| File | What it does |
|---|---|
| `patches/0001-llama-littlebit-factorized-linear.patch` | ~100-line llama.cpp patch. The loader accepts `<w>.lb_*` factor tensors in place of a dense `<w>.weight`; `build_lora_mm` (the shared matmul every architecture uses) runs `s_out ⊙ (U · (s_mid ⊙ (V · (s_in ⊙ x))))`. No new ggml ops or kernels: the binary factors use llama.cpp's existing **Q1_0** (1.125 bpw, ARM NEON kernels) or **TQ1_0**. |
| `convert_littlebit_to_gguf.py` | LittleBit checkpoint (packed `*_packed` format from the official code, or raw latent `U/V`) → GGUF. Tokenizer and metadata go through llama.cpp's own `convert_hf_to_gguf.py`. Pads the latent dimension to the quant block size, with the padding cancelled exactly by zero scales. |
| `scripts/build_desktop.sh` | Clone llama.cpp at the tested release, apply the patch, build (CPU; pass `-DGGML_VULKAN=ON`, `-DGGML_CUDA=ON`, …). |
| `scripts/build_android_ndk.sh` | Cross-compile for Android arm64 on a PC with the NDK. |
| `scripts/termux_build.sh` | Build directly on the phone in Termux. |
| `scripts/build_ollama.sh` | Build Ollama from source with the patch dropped into `llama/compat/`. |
| `ollama/Modelfile` | Minimal Modelfile. |
| `tests/` | Parity test: build a tiny model with the **official** LittleBit code, then compare PyTorch logits against patched llama.cpp. |

## Quick start

```bash
# 1. build patched llama.cpp (on your PC)
bash scripts/build_desktop.sh
LLAMA=build/llama.cpp

# 2. convert a LittleBit checkpoint (local dir or HF repo id)
pip install -r $LLAMA/requirements/requirements-convert_hf_to_gguf.txt
python convert_littlebit_to_gguf.py LittleBitLLM/littlebit-qwen3-8b-0.55bpw \
    --llama-cpp $LLAMA --outfile qwen3-8b-lb055.gguf

# 3. (optional, recommended for phones) shrink the embedding table and lm_head with stock llama-quantize.
#    It only touches *.weight tensors, so the LittleBit factors pass through unchanged.
$LLAMA/build/bin/llama-quantize --allow-requantize --token-embedding-type q4_k --output-tensor-type q4_k \
    qwen3-8b-lb055.gguf qwen3-8b-lb055-small.gguf Q4_K_M

# 4. run it on the PC
$LLAMA/build/bin/llama-cli -m qwen3-8b-lb055-small.gguf -c 2048
```

On the phone, pick one:

- **Termux (easiest, no PC toolchain):** copy this folder to the phone, run `bash scripts/termux_build.sh`,
  put the `.gguf` in `~/`, then `~/llama.cpp-littlebit/build/bin/llama-server -m ~/model.gguf -c 2048`
  and open `http://127.0.0.1:8080` in the phone browser.
- **NDK + adb:** `ANDROID_NDK=… bash scripts/build_android_ndk.sh`, then follow the printed `adb push` lines.
- **Ollama:** `bash scripts/build_ollama.sh`, put the GGUF next to `ollama/Modelfile` as `model.gguf`,
  run `ollama create littlebit -f ollama/Modelfile`. A stock Ollama (desktop app, `pkg install ollama`
  in Termux) does **not** have the patch and rejects the file.

Phone apps that bundle their own llama.cpp (PocketPal, ChatterUI, LM Studio, …) can't load these files
until they ship this patch.

### Converter options that matter on phones

- `--embd-type` (default `q8_0`, also `q4_0`, `f16`, `bf16`): at sub-1-bit, the **token embedding and lm_head
  are most of the file** (Qwen3-8B has a 152k vocabulary). For k-quants use the `llama-quantize` step above.
- `--bin-type` (default `q1_0`): `q1_0` stores ±1 at 1.125 bpw. `tq1_0` (1.69 bpw) is a fallback for
  builds where Q1_0 is slow on your backend. `f16` exists only for debugging and gives exact parity.

## Results (measured on x86-64, 4 cores, CPU only; no phone was available to test)

**Parity with the official PyTorch implementation** (tiny random models made with SamsungLabs' code,
Dual-SVID + Joint-ITQ init, residual on, 8 tokens):

| Case | cosine(logits) | top-1 match |
|---|---|---|
| Qwen3, `--bin-type f16` | 1.000000 | 8/8 |
| Qwen3, `q1_0` (default) | 0.99990 | 7/8 (one near-tie) |
| Qwen3, `tq1_0` | 0.99982 | 8/8 |
| Qwen3, no residual, 0.55 bpw, `q1_0` | 0.99977 | 8/8 |
| Llama (tests the q/k row permutation), `q1_0` | 0.99991 | 8/8 |

With `f16` factors the match is exact, so the graph math is right. The small gap with `q1_0`/`tq1_0`
comes from llama.cpp quantizing *activations* to 8 bits inside those matmuls.

**Real model:** [`LittleBitLLM/littlebit-qwen3-8b-0.55bpw`](https://huggingface.co/LittleBitLLM/littlebit-qwen3-8b-0.55bpw)
(community LittleBit-2 QAT checkpoint, 0.5 epoch):

| File | Size | Wikitext-2 PPL (llama.cpp, ctx 2048, 2 windows) | Speed, gen / prompt (tok/s) |
|---|---:|---:|---:|
| converter default (Q1_0 factors, Q8_0 embd + head) | 1,842 MiB | 26.77 (22.88 over 6 windows) | 10.2 / 25.4 |
| + `llama-quantize Q4_K_M` (Q4_K embd, Q6_K head) | 1,402 MiB | 26.90 | 9.4 / 25.5 |
| + `--token-embedding-type q4_k --output-tensor-type q4_k` | 1,249 MiB | 27.48 | – |

For scale: the original Qwen3-8B scores PPL 9.73 and this checkpoint 24.48 in PyTorch (both from the
model card, full test set, a different windowing from llama.cpp's, so the numbers are only roughly
comparable). An ordinary Qwen3-8B Q4_K_M GGUF is about 5 GB. The LittleBit layers themselves take
**574 MiB for 6.95 B weights (0.69 bits/weight on disk)**. The rest is the embedding table and lm_head.

The same file also ran through **Ollama** built by `scripts/build_ollama.sh` (llama.cpp b12257 + Ollama's hooks
+ this patch). Greedy output was identical to llama.cpp ("The capital of France is Paris, and the city is
known for its vibrant culture…"), at 7.5 tok/s while another benchmark shared the CPU.

## Limitations, and the counter-arguments

- **You need a LittleBit-trained checkpoint.** Turning a model of your own into LittleBit means QAT on a
  big GPU (`python -m main ... --use_itq True` in the upstream repo). Converting the init alone
  (no training) produces garbage at sub-1-bit; the tiny test models are only useful for parity checks.
- **Quality vs. ordinary 2–4-bit quants.** The 0.55-bpw 8B above scores far below the original
  (its model card: ARC-Challenge at chance). On a phone, a *smaller* model at Q4 may beat a *bigger*
  model at 0.55 bpw at the same file size. Compare them on your own task before you commit.
- **Speed claims.** The papers report kernel-level speedups from custom 1-bit GEMV kernels. This
  patch reuses llama.cpp's generic Q1_0/TQ1_0 kernels and runs two thin matmuls (four with the residual)
  instead of one, so expect *memory* savings first. Phone speed was not measured here.
- **Architectures.** The converter is tested on Qwen3 and Llama, and allows Qwen2 and Mistral (same code path).
  Others work only if llama.cpp's converter doesn't transform their linear weights (`--force`).
  MoE expert weights (`mul_mat_id`) and fused-QKV models are not supported.
- **GPU backends.** Only the CPU backend was tested. Q1_0 has Vulkan, OpenCL and Metal code paths upstream,
  but the mixed-type scale multiplies were not checked there.
- **License.** LittleBit code and the community checkpoints are **CC BY-NC 4.0** (non-commercial).
  The patch and converter here don't copy LittleBit code; they only read its checkpoint format.

## Re-running the parity test

```bash
git clone https://github.com/SamsungLabs/LittleBit
pip install torch "transformers>=4.51" safetensors huggingface_hub
python tests/make_tiny_littlebit.py --littlebit LittleBit --tokenizer-from Qwen/Qwen3-0.6B --out tiny
python convert_littlebit_to_gguf.py tiny --llama-cpp build/llama.cpp --outfile tiny.gguf --embd-type f16
g++ -O2 -std=c++17 tests/dump_logits.cpp -Ibuild/llama.cpp/include -Ibuild/llama.cpp/ggml/include \
    -Lbuild/llama.cpp/build/bin -lllama -lggml -lggml-base -Wl,-rpath,$PWD/build/llama.cpp/build/bin -o dump_logits
./dump_logits tiny.gguf out.f32 $(cat tiny/ref_tokens.txt)
python tests/compare_logits.py tiny/ref_logits.f32 out.f32
```
