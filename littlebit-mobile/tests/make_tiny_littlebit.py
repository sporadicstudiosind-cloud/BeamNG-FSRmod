#!/usr/bin/env python3
"""
Build a tiny random model, compress it with the *official* LittleBit code (Dual-SVID + LittleBit-2 Joint-ITQ init,
no QAT), save it in the packed LittleBit checkpoint format, and record reference logits from the PyTorch forward.

This exercises the whole pipeline (packing format, residual branch, latent padding, name mapping, llama.cpp graph)
without needing a GPU or a big download. The model is random, so only numerical parity matters.

usage: make_tiny_littlebit.py --littlebit /path/to/SamsungLabs/LittleBit --tokenizer-from Qwen/Qwen3-0.6B \
           --arch qwen3 --out /tmp/tiny-lb [--eff-bit 1.0] [--no-residual] [--tokens 1,2,3]
"""
import argparse
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import torch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--littlebit", type=Path, required=True, help="checkout of github.com/SamsungLabs/LittleBit")
    ap.add_argument("--tokenizer-from", required=True, help="HF repo whose tokenizer files to copy")
    ap.add_argument("--arch", choices=["qwen3", "llama"], default="qwen3")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--eff-bit", type=float, default=1.0)
    ap.add_argument("--no-residual", action="store_true")
    ap.add_argument("--no-itq", action="store_true")
    ap.add_argument("--hidden", type=int, default=256)
    ap.add_argument("--tokens", default="9707,11,1879,0,358,1079,264,1273")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    sys.path.insert(0, str(args.littlebit))
    from quantization.utils.quant_util import apply_littlebit_patch
    from huggingface_hub import snapshot_download
    from transformers import AutoConfig, AutoModelForCausalLM
    from safetensors.torch import save_file

    torch.manual_seed(args.seed)
    tok_dir = Path(snapshot_download(args.tokenizer_from, allow_patterns=["tokenizer*", "vocab*", "merges*", "special_tokens_map.json", "*.model", "config.json", "generation_config.json"]))
    base_cfg = json.loads((tok_dir / "config.json").read_text())

    h = args.hidden
    common = dict(hidden_size=h, intermediate_size=2 * h + 128, num_hidden_layers=2, num_attention_heads=4,
                  num_key_value_heads=2, vocab_size=base_cfg["vocab_size"], tie_word_embeddings=False,
                  max_position_embeddings=2048, rms_norm_eps=1e-6, torch_dtype="float32")
    if args.arch == "qwen3":
        cfg = AutoConfig.for_model("qwen3", head_dim=h // 4, rope_theta=1e6, **common)
    else:
        cfg = AutoConfig.for_model("llama", rope_theta=1e4, **common)
    for k in ("bos_token_id", "eos_token_id"):
        if k in base_cfg:
            setattr(cfg, k, base_cfg[k])

    model = AutoModelForCausalLM.from_config(cfg, torch_dtype=torch.float32).eval()
    # make the norms non-trivial so a wrong mapping shows up
    with torch.no_grad():
        for n, p in model.named_parameters():
            if "norm" in n:
                p.uniform_(0.5, 1.5)

    qargs = SimpleNamespace(quant_func="STEBinary", quant_mod="LittleBitLinear", residual=not args.no_residual,
                            split_dim=1024, eff_bit=args.eff_bit, min_split_dim=8, use_itq=not args.no_itq,
                            itq_n_iter=20, kv_factor=1.0, model_id="tiny")
    apply_littlebit_patch(model, qargs, do_train=True)
    model.eval()

    tokens = [int(t) for t in args.tokens.split(",")]
    with torch.no_grad():
        logits = model(torch.tensor([tokens])).logits[0].float().numpy()

    args.out.mkdir(parents=True, exist_ok=True)
    sd = {k: v.detach().contiguous() for k, v in model.state_dict().items()}
    save_file(sd, str(args.out / "model.safetensors"), metadata={"format": "pt"})

    cfg_d = cfg.to_dict()
    cfg_d["architectures"] = ["Qwen3ForCausalLM" if args.arch == "qwen3" else "LlamaForCausalLM"]
    cfg_d.update(quant_func="STEBinary", quant_mod="LittleBitLinear", eff_bit=args.eff_bit,
                 residual=not args.no_residual, use_itq=not args.no_itq, split_dim=1024)
    (args.out / "config.json").write_text(json.dumps(cfg_d, indent=2))
    for f in tok_dir.iterdir():
        if f.name not in ("config.json",) and f.is_file():
            (args.out / f.name).write_bytes(f.read_bytes())

    logits.astype("float32").tofile(args.out / "ref_logits.f32")
    (args.out / "ref_tokens.txt").write_text(" ".join(map(str, tokens)))

    lb = [m for m in model.modules() if type(m).__name__ == "LittleBitLinear"]
    print(f"saved {args.out}: {len(lb)} LittleBit layers, split_dim={[int(m.split_dim) for m in lb][:4]}..., "
          f"eff_bit_actual={[round(m.eff_bit_actual, 3) for m in lb][:4]}...")


if __name__ == "__main__":
    main()
