#!/usr/bin/env python3
"""Psychoacoustic oracle (extension-3 §8/§9 M12).

Runs the pinned MoSQITo over the calibrated psycho fixtures plus a fixed subset of the v1
fixtures and writes tests/golden/psycho_reference.json. MoSQITo is a development-time oracle
only: its numbers are ground truth for the C++ implementation's gates; no code is ported.

Calibration (extension-3 §4): p[n] = K * x[n], K = p0 * 10^((ref_spl + 23) / 20), p0 = 20 uPa.
A -23 LUFS signal is assumed to play at ref_spl dB SPL (default 75).

The script asserts MoSQITo's own values against the §5 definitional rows before writing —
an oracle that contradicts the unit definitions must fail loudly, not be committed.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from importlib.metadata import version as pkg_version

from mosqito.sq_metrics import (
    loudness_zwst,
    loudness_zwtv,
    roughness_dw,
    sharpness_din_st,
)

# Fixed v1-fixture subset (extension-3 §8); paths relative to the v1 fixtures dir.
V1_SUBSET = [
    "sine997_cal.wav",
    "noise_white_1s.wav",
    "click.wav",
    "decay_t60.wav",
    "darkset/dark_00.wav",
    "bright_outlier.wav",
]

PSYCHO_FILES = [
    "tone1k_40db.wav",
    "tone1k_50db.wav",
    "tone1k_60db.wav",
    "am70_60db.wav",
    "am4_60db.wav",
    "nbnoise1k_60db.wav",
]


def load_calibrated(path: Path, k: float) -> tuple[np.ndarray, int]:
    fs, x = wavfile.read(path)
    if x.ndim != 1:
        x = x[:, 0]
    if x.dtype != np.float32 and x.dtype != np.float64:
        x = x.astype(np.float64) / np.iinfo(x.dtype).max
    return x.astype(np.float64) * k, int(fs)


def analyze(path: Path, k: float) -> dict:
    p, fs = load_calibrated(path, k)
    n_st, _, _ = loudness_zwst(p, fs)
    n_tv, _, _, _t = loudness_zwtv(p, fs)
    n_tv = np.asarray(n_tv, dtype=np.float64)
    sharp = sharpness_din_st(p, fs)
    r_track = np.asarray(roughness_dw(p, fs)[0], dtype=np.float64)
    return {
        "loudness_zwst_sone": round(float(n_st), 4),
        "loudness_zwtv_n5_sone": round(float(np.percentile(n_tv, 95)), 4),
        "loudness_zwtv_mean_sone": round(float(np.mean(n_tv)), 4),
        "sharpness_din_acum": round(float(sharp), 4),
        "roughness_dw_asper": round(float(np.mean(r_track)), 4),
    }


def assert_definitional(results: dict) -> list[str]:
    """§5 definitional rows, applied to the oracle's own numbers."""
    checks = []

    def check(name, ok, detail):
        checks.append(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}")
        return ok

    ok = True
    n40 = results["tone1k_40db.wav"]["loudness_zwst_sone"]
    n50 = results["tone1k_50db.wav"]["loudness_zwst_sone"]
    ok &= check("1 sone anchor", abs(n40 - 1.0) <= 0.08, f"40 dB tone = {n40} sone")
    ratio = n50 / n40
    ok &= check("10 dB doubling", 1.7 <= ratio <= 2.3, f"50/40 dB ratio = {ratio:.3f}")
    s = results["nbnoise1k_60db.wav"]["sharpness_din_acum"]
    ok &= check("1 acum anchor", abs(s - 1.0) <= 0.10, f"narrowband noise = {s} acum")
    r70 = results["am70_60db.wav"]["roughness_dw_asper"]
    ok &= check("1 asper anchor", abs(r70 - 1.0) <= 0.20, f"70 Hz AM = {r70} asper")
    r4 = results["am4_60db.wav"]["roughness_dw_asper"]
    ok &= check("4 Hz AM not rough", r4 < 0.3, f"4 Hz AM = {r4} asper")
    r_tone = results["tone1k_60db.wav"]["roughness_dw_asper"]
    ok &= check("pure tone not rough", r_tone < 0.1, f"unmodulated tone = {r_tone} asper")
    sharp_bright = results["bright_outlier.wav"]["sharpness_din_acum"]
    sharp_dark = results["darkset/dark_00.wav"]["sharpness_din_acum"]
    ok &= check("sharpness direction", sharp_bright > sharp_dark,
                f"bright_outlier {sharp_bright} > dark_00 {sharp_dark}")
    if not ok:
        raise SystemExit("oracle FAILED its own definitional gates:\n" + "\n".join(checks))
    return checks


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("psycho_dir", type=Path)
    ap.add_argument("v1_fixtures_dir", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--ref-spl", type=float, default=75.0)
    args = ap.parse_args()

    k = 20e-6 * 10 ** ((args.ref_spl + 23.0) / 20.0)

    results = {}
    for name in PSYCHO_FILES:
        results[name] = analyze(args.psycho_dir / name, k)
        print(f"{name}: {results[name]}")
    for name in V1_SUBSET:
        results[name] = analyze(args.v1_fixtures_dir / name, k)
        print(f"{name}: {results[name]}")

    checks = assert_definitional(results)
    print("\n".join(checks))

    out = {
        "_meta": {
            "tool": "tools/psycho_oracle/compute_reference.py",
            "mosqito": pkg_version("mosqito"),
            "numpy": np.__version__,
            "scipy": pkg_version("scipy"),
            "ref_spl": args.ref_spl,
            "calibration_pa_per_fs": round(k, 6),
            "n5_definition": "95th percentile of the loudness_zwtv track",
            "fluctuation_strength": "not provided by mosqito 1.2.1; definitional gates only "
                                    "(extension-3 §5)",
        },
        "files": results,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
