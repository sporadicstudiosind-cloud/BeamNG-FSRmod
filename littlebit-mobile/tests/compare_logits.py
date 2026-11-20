#!/usr/bin/env python3
"""compare_logits.py ref.f32 got.f32 : per-token cosine / max-abs / top-1 between two raw float32 logit dumps."""
import sys

import numpy as np


def main():
    ref = np.fromfile(sys.argv[1], dtype=np.float32)
    got = np.fromfile(sys.argv[2], dtype=np.float32)
    assert ref.size == got.size, f"size mismatch {ref.size} vs {got.size}"
    if len(sys.argv) > 3:
        n_tok = int(sys.argv[3])
    else:  # make_tiny_littlebit.py writes the token list next to the reference logits
        from pathlib import Path
        n_tok = len((Path(sys.argv[1]).parent / "ref_tokens.txt").read_text().split())
    ref, got = ref.reshape(n_tok, -1), got.reshape(n_tok, -1)
    worst = 1.0
    for i, (r, g) in enumerate(zip(ref, got)):
        cos = float(r @ g / (np.linalg.norm(r) * np.linalg.norm(g)))
        worst = min(worst, cos)
        print(f"token {i}: cos={cos:.6f} max|d|={np.abs(r - g).max():.4f} top1 {r.argmax()} / {g.argmax()}")
    print(f"worst cosine {worst:.6f}")
    sys.exit(0 if worst > 0.999 else 1)


if __name__ == "__main__":
    main()
