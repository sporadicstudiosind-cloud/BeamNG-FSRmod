#!/usr/bin/env python3
"""
Convert a LittleBit / LittleBit-2 checkpoint (SamsungLabs/LittleBit format) to GGUF
for the patched llama.cpp in this folder (see patches/).

A LittleBit linear layer stores
    W ~= diag(u1) * sign(U) * diag(u2 * v1) * sign(V) * diag(v2)      [+ same form for the residual branch]
with sign(U) of shape [out, lat] and sign(V) of shape [lat, in].

The GGUF keeps that factorized form, so the file stays sub-1-bit-sized:
    <name>.lb_v      binary V    [lat_pad, in]   Q1_0 (1.125 bpw) or TQ1_0 (1.69 bpw)
    <name>.lb_u      binary U    [out, lat_pad]  Q1_0 / TQ1_0
    <name>.lb_s_in   v2          [in]            F32
    <name>.lb_s_mid  v1 * u2     [lat_pad]       F32 (zero in the padding, which cancels the padded rows/cols)
    <name>.lb_s_out  u1          [out]           F32
and lb_r_* for the residual branch.

Everything that is not a LittleBit layer (embeddings, norms, lm_head, tokenizer, hyper-parameters)
is converted by llama.cpp's own convert_hf_to_gguf.py, so tokenizer handling stays upstream.

Usage:
    python convert_littlebit_to_gguf.py <checkpoint dir or HF repo id> \
        --llama-cpp /path/to/llama.cpp --outfile model.gguf [--bin-type q1_0] [--embd-type q4_0]
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

logger = logging.getLogger("littlebit-to-gguf")

# architectures whose converter does not transform linear weights (or whose transform we replicate below)
SUPPORTED_ARCHS = {
    "Qwen3ForCausalLM", "Qwen2ForCausalLM", "LlamaForCausalLM", "MistralForCausalLM",
}
# llama.cpp's LLaMA converter permutes q/k rows (HF rotary layout -> GGUF layout)
PERMUTE_QK_ARCHS = {"LlamaForCausalLM", "MistralForCausalLM"}

LB_PARAMS = ("U", "V", "u1", "u2", "v1", "v2")
LB_BUFFERS = ("_eff_bit_target", "_split_dim_final", "_eff_bit_actual")


# ----------------------------------------------------------------------------- checkpoint reading

def unpack_bits(packed: np.ndarray, shape: tuple[int, int]) -> np.ndarray:
    """Inverse of LittleBit's binary_packer: int32 words, LSB first, bit 1 = -1, bit 0 = +1."""
    n_rows, n_cols = shape
    words = packed.astype(np.int32).view(np.uint32)
    assert words.shape == (n_rows, (n_cols + 31) // 32), f"packed shape {words.shape} does not match {shape}"
    bits = np.unpackbits(words.view(np.uint8).reshape(n_rows, -1), axis=1, bitorder="little")
    # little-endian uint32 -> bytes are LSB first, so bit k of word w lands at column 32*w + k
    bits = bits[:, :n_cols]
    return (1 - 2 * bits.astype(np.int8)).astype(np.int8)


def load_state_dict(model_dir: Path) -> dict[str, np.ndarray]:
    from safetensors import safe_open
    import torch

    files: list[Path]
    index = model_dir / "model.safetensors.index.json"
    if index.is_file():
        weight_map = json.loads(index.read_text())["weight_map"]
        files = sorted({model_dir / f for f in weight_map.values()})
    else:
        files = sorted(model_dir.glob("*.safetensors"))
    if not files:
        raise FileNotFoundError(f"no .safetensors files in {model_dir}")

    sd: dict[str, "torch.Tensor"] = {}
    for f in files:
        with safe_open(str(f), framework="pt", device="cpu") as fh:
            for k in fh.keys():
                sd[k] = fh.get_tensor(k)
    return sd


def find_littlebit_layers(sd) -> dict[str, dict]:
    """Group LittleBit tensors by layer prefix. Supports packed (U_packed/U_shape) and raw latent (U float) formats."""
    layers: dict[str, dict] = {}
    pat = re.compile(r"^(.*)\.(U|V|U_R|V_R)_packed$")
    for k in sd:
        m = pat.match(k)
        if m:
            layers.setdefault(m.group(1), {})
    if not layers:
        # unpacked training checkpoint: <prefix>.U, <prefix>.V, <prefix>.u1 ...
        for k in sd:
            if k.endswith(".U") and k[:-2] + ".v2" in sd:
                layers.setdefault(k[:-2], {})
    return layers


def binary_factor(sd, prefix: str, name: str) -> np.ndarray | None:
    if f"{prefix}.{name}_packed" in sd:
        packed = sd[f"{prefix}.{name}_packed"].numpy()
        shape = tuple(int(x) for x in sd[f"{prefix}.{name}_shape"].tolist())
        return unpack_bits(packed, shape)
    if f"{prefix}.{name}" in sd:
        t = sd[f"{prefix}.{name}"].float().numpy()
        return np.where(t >= 0, 1, -1).astype(np.int8)  # same as STEBinary/SmoothSign forward (sign, 0 -> +1)
    return None


def vec(sd, key: str) -> np.ndarray:
    return sd[key].float().numpy().reshape(-1).astype(np.float32)


def consumed_keys(prefix: str) -> list[str]:
    keys = []
    for suf in ("", "_R"):
        for p in LB_PARAMS:
            keys.append(f"{prefix}.{p}{suf}")
        for p in ("U", "V"):
            keys += [f"{prefix}.{p}{suf}_packed", f"{prefix}.{p}{suf}_shape"]
    keys += [f"{prefix}.{b}" for b in LB_BUFFERS]
    return keys


# ----------------------------------------------------------------------------- GGUF helpers

def llama_permute_rows(x: np.ndarray, n_head: int) -> np.ndarray:
    """Same as LlamaModel.permute in llama.cpp's converter, applied to the leading (output) axis."""
    rest = x.shape[1:]
    return (x.reshape(n_head, 2, x.shape[0] // n_head // 2, *rest)
             .swapaxes(1, 2)
             .reshape(x.shape))


def pick_bin_type(gguf, requested: str, row_len: int):
    q = {"q1_0": gguf.GGMLQuantizationType.Q1_0, "tq1_0": gguf.GGMLQuantizationType.TQ1_0,
         "q8_0": gguf.GGMLQuantizationType.Q8_0, "f16": gguf.GGMLQuantizationType.F16}[requested]
    if q == gguf.GGMLQuantizationType.F16:
        return q
    block = gguf.GGML_QUANT_SIZES[q][0]
    if row_len % block == 0:
        return q
    for fallback in (gguf.GGMLQuantizationType.Q8_0, gguf.GGMLQuantizationType.F16):
        if fallback == gguf.GGMLQuantizationType.F16 or row_len % gguf.GGML_QUANT_SIZES[fallback][0] == 0:
            logger.warning(f"row length {row_len} is not a multiple of {block}, storing that factor as {fallback.name}")
            return fallback
    return gguf.GGMLQuantizationType.F16


def lat_padding(gguf, requested: str) -> int:
    if requested == "q1_0":
        return gguf.GGML_QUANT_SIZES[gguf.GGMLQuantizationType.Q1_0][0]
    if requested == "tq1_0":
        return gguf.GGML_QUANT_SIZES[gguf.GGMLQuantizationType.TQ1_0][0]
    if requested == "q8_0":
        return 32
    return 1


def quantize(gguf, arr: np.ndarray, qtype) -> np.ndarray:
    if qtype == gguf.GGMLQuantizationType.F32:
        return arr.astype(np.float32)
    if qtype == gguf.GGMLQuantizationType.F16:
        return arr.astype(np.float16)
    return gguf.quants.quantize(arr.astype(np.float32), qtype)


# ----------------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model", help="LittleBit checkpoint directory or Hugging Face repo id")
    ap.add_argument("--llama-cpp", type=Path, required=True, help="path to a llama.cpp checkout (for convert_hf_to_gguf.py and gguf-py)")
    ap.add_argument("--outfile", type=Path, required=True)
    ap.add_argument("--bin-type", choices=["q1_0", "tq1_0", "q8_0", "f16"], default="q1_0",
                    help="storage for the binary factors. q1_0 = 1.125 bpw (default), tq1_0 = 1.69 bpw, q8_0/f16 for debugging")
    ap.add_argument("--embd-type", choices=["f16", "bf16", "q8_0", "q6_k", "q4_k", "q4_0"], default="q8_0",
                    help="type for token_embd/output (these dominate the file size at sub-1-bit). default q8_0")
    ap.add_argument("--dense-dtype", choices=["f16", "bf16", "f32"], default="f16",
                    help="outtype passed to convert_hf_to_gguf.py for the non-LittleBit tensors")
    ap.add_argument("--force", action="store_true", help="convert even if the architecture is not in the tested list")
    ap.add_argument("--keep-temp", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(levelname)s %(message)s")

    llama_cpp = args.llama_cpp.resolve()
    sys.path.insert(0, str(llama_cpp / "gguf-py"))
    import gguf  # noqa: E402

    model_dir = Path(args.model)
    if not model_dir.is_dir():
        from huggingface_hub import snapshot_download
        logger.info(f"downloading {args.model} from the Hugging Face Hub")
        model_dir = Path(snapshot_download(args.model, allow_patterns=["*.json", "*.safetensors", "*.txt", "*.model", "*.tiktoken", "*.jinja"]))

    config = json.loads((model_dir / "config.json").read_text())
    arch = (config.get("architectures") or ["?"])[0]
    if arch not in SUPPORTED_ARCHS and not args.force:
        logger.error(f"architecture {arch} has not been tested; supported: {sorted(SUPPORTED_ARCHS)} (use --force to try anyway)")
        return 1

    logger.info(f"loading {model_dir}")
    sd = load_state_dict(model_dir)
    layers = find_littlebit_layers(sd)
    if not layers:
        logger.error("no LittleBit layers found (expected *.U_packed tensors)")
        return 1
    logger.info(f"found {len(layers)} LittleBit layers")

    tmp = Path(tempfile.mkdtemp(prefix="littlebit-gguf-"))
    try:
        # ---- stage 1: dense skeleton -> llama.cpp's own converter
        skel = tmp / "skeleton"
        skel.mkdir()
        for f in model_dir.iterdir():
            if f.is_file() and f.suffix != ".safetensors" and not f.name.endswith(".safetensors.index.json") and f.name != "config.json":
                shutil.copy(f, skel / f.name)
        cfg = {k: v for k, v in config.items() if k != "auto_map"}
        (skel / "config.json").write_text(json.dumps(cfg, indent=2))

        drop = set()
        for p in layers:
            drop.update(consumed_keys(p))
        dense = {k: v.contiguous() for k, v in sd.items() if k not in drop}
        from safetensors.torch import save_file
        save_file(dense, str(skel / "model.safetensors"), metadata={"format": "pt"})
        del dense

        skel_gguf = tmp / "skeleton.gguf"
        cmd = [sys.executable, str(llama_cpp / "convert_hf_to_gguf.py"), str(skel), "--outfile", str(skel_gguf), "--outtype", args.dense_dtype]
        logger.info("running " + " ".join(cmd))
        env = dict(os.environ)
        subprocess.run(cmd, check=True, env=env)

        # ---- stage 2: copy skeleton + add factorized layers
        reader = gguf.GGUFReader(skel_gguf)
        arch_name = reader.fields[gguf.Keys.General.ARCHITECTURE].contents()
        n_block = int(reader.fields[f"{arch_name}.block_count"].contents())
        arch_enum = next(k for k, v in gguf.MODEL_ARCH_NAMES.items() if v == arch_name)
        tmap = gguf.get_tensor_name_map(arch_enum, n_block)

        writer = gguf.GGUFWriter(args.outfile, arch_name)
        for field in reader.fields.values():
            if field.name == gguf.Keys.General.ARCHITECTURE or field.name.startswith("GGUF."):
                continue
            vtype = field.types[0]
            sub = field.types[-1] if vtype == gguf.GGUFValueType.ARRAY else None
            writer.add_key_value(field.name, field.contents(), vtype, sub_type=sub)
        writer.add_string("littlebit.version", "1")
        writer.add_string("littlebit.bin_type", args.bin_type)
        for k in ("eff_bit", "residual", "use_itq", "split_dim"):
            if k in config:
                writer.add_string(f"littlebit.{k}", str(config[k]))

        embd_q = {"f16": gguf.GGMLQuantizationType.F16, "bf16": gguf.GGMLQuantizationType.BF16,
                  "q8_0": gguf.GGMLQuantizationType.Q8_0, "q6_k": gguf.GGMLQuantizationType.Q6_K,
                  "q4_k": gguf.GGMLQuantizationType.Q4_K, "q4_0": gguf.GGMLQuantizationType.Q4_0}[args.embd_type]

        n_bytes = {"dense": 0, "embd": 0, "lb": 0}
        for t in reader.tensors:
            if t.name in ("token_embd.weight", "output.weight") and embd_q != t.tensor_type:
                deq = gguf.quants.dequantize(t.data, t.tensor_type).astype(np.float32)
                blk = gguf.GGML_QUANT_SIZES[embd_q][0]
                q = embd_q if deq.shape[-1] % blk == 0 else gguf.GGMLQuantizationType.F16
                data = quantize(gguf, deq, q)
                writer.add_tensor(t.name, data, raw_dtype=q)
                n_bytes["embd"] += data.nbytes
            else:
                writer.add_tensor(t.name, t.data, raw_dtype=t.tensor_type)
                n_bytes["embd" if t.name in ("token_embd.weight", "output.weight") else "dense"] += t.data.nbytes

        n_head = config.get("num_attention_heads")
        n_head_kv = config.get("num_key_value_heads", n_head)
        pad_to = lat_padding(gguf, args.bin_type)
        n_weights = 0

        for prefix in sorted(layers):
            gname = tmap.get_name(prefix + ".weight", try_suffixes=(".weight",))
            if gname is None:
                raise ValueError(f"cannot map {prefix} to a GGUF tensor name for arch {arch_name}")
            base = gname[: -len("weight")]

            for suf, tag in (("", "lb_"), ("_R", "lb_r_")):
                U = binary_factor(sd, prefix, "U" + suf)
                V = binary_factor(sd, prefix, "V" + suf)
                if U is None or V is None:
                    if suf == "":
                        raise ValueError(f"{prefix}: missing U/V")
                    continue
                u1 = vec(sd, f"{prefix}.u1{suf}")
                u2 = vec(sd, f"{prefix}.u2{suf}")
                v1 = vec(sd, f"{prefix}.v1{suf}")
                v2 = vec(sd, f"{prefix}.v2{suf}")
                n_out, n_lat = U.shape
                n_lat2, n_in = V.shape
                assert n_lat == n_lat2 and u2.size == n_lat and v1.size == n_lat and u1.size == n_out and v2.size == n_in

                s_mid = v1 * u2

                # LLaMA-style converters permute the q/k output rows; do the same on U rows and u1
                if arch in PERMUTE_QK_ARCHS:
                    if prefix.endswith("q_proj"):
                        U, u1 = llama_permute_rows(U, n_head), llama_permute_rows(u1, n_head)
                    elif prefix.endswith("k_proj"):
                        U, u1 = llama_permute_rows(U, n_head_kv), llama_permute_rows(u1, n_head_kv)

                # pad the latent dim to the quant block size: padded rows of V / cols of U are +1, s_mid is 0 there
                lat_pad = -(-n_lat // pad_to) * pad_to
                if lat_pad != n_lat:
                    U = np.concatenate([U, np.ones((n_out, lat_pad - n_lat), np.int8)], axis=1)
                    V = np.concatenate([V, np.ones((lat_pad - n_lat, n_in), np.int8)], axis=0)
                    s_mid = np.concatenate([s_mid, np.zeros(lat_pad - n_lat, np.float32)])

                qv = pick_bin_type(gguf, args.bin_type, n_in)
                qu = pick_bin_type(gguf, args.bin_type, lat_pad)
                dv = quantize(gguf, V.astype(np.float32), qv)
                du = quantize(gguf, U.astype(np.float32), qu)
                writer.add_tensor(base + tag + "v", dv, raw_dtype=qv)
                writer.add_tensor(base + tag + "u", du, raw_dtype=qu)
                writer.add_tensor(base + tag + "s_in", v2)
                writer.add_tensor(base + tag + "s_mid", s_mid.astype(np.float32))
                writer.add_tensor(base + tag + "s_out", u1.astype(np.float32))
                n_bytes["lb"] += dv.nbytes + du.nbytes + 4 * (v2.size + s_mid.size + u1.size)
            n_weights += n_out * n_in

        logger.info(f"writing {args.outfile}")
        writer.write_header_to_file()
        writer.write_kv_data_to_file()
        writer.write_tensors_to_file(progress=True)
        writer.close()

        mib = lambda b: b / 2**20  # noqa: E731
        logger.info(f"LittleBit layers: {mib(n_bytes['lb']):.1f} MiB for {n_weights/1e9:.3f} B weights "
                    f"= {8 * n_bytes['lb'] / n_weights:.3f} bits/weight on disk")
        logger.info(f"embeddings/lm_head: {mib(n_bytes['embd']):.1f} MiB ({args.embd_type}), other dense: {mib(n_bytes['dense']):.1f} MiB")
        logger.info(f"total file: {mib(args.outfile.stat().st_size):.1f} MiB")
    finally:
        if args.keep_temp:
            logger.info(f"kept temp dir {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
