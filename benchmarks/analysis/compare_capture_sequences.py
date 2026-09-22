#!/usr/bin/env python3
"""Compare two dlssnr-repro RGBA16F output sequences."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def load_rgba16f(path: Path, expected_texels: int) -> tuple[np.ndarray, np.ndarray]:
    bits = np.fromfile(path, dtype="<u2")
    expected_components = expected_texels * 4
    if bits.size != expected_components:
        raise ValueError(
            f"{path}: expected {expected_components} components, found {bits.size}"
        )
    return bits.reshape(expected_texels, 4), bits.view("<f2").reshape(expected_texels, 4).astype(np.float32)


def distribution(rgb: np.ndarray) -> dict[str, object]:
    return {
        "exact_zero_rgb_texels": int(np.count_nonzero(np.all(rgb == 0, axis=1))),
        "exact_zero_components_rgb": np.count_nonzero(rgb == 0, axis=0).tolist(),
        "negative_components_rgb": np.count_nonzero(rgb < 0, axis=0).tolist(),
        "greater_than_one_components_rgb": np.count_nonzero(rgb > 1, axis=0).tolist(),
        "minimum_rgb": np.min(rgb, axis=0).tolist(),
        "maximum_rgb": np.max(rgb, axis=0).tolist(),
        "mean_rgb": np.mean(rgb, axis=0, dtype=np.float64).tolist(),
        "mean_absolute_rgb": np.mean(np.abs(rgb), axis=0, dtype=np.float64).tolist(),
    }


def error_metrics(reference: np.ndarray, candidate: np.ndarray, peak: float) -> dict[str, object]:
    error = candidate - reference
    absolute_error = np.abs(error)
    mse = np.mean(np.square(error), axis=0, dtype=np.float64)
    rmse = np.sqrt(mse)
    global_mse = float(np.mean(np.square(error), dtype=np.float64))
    global_rmse = math.sqrt(global_mse)
    return {
        "mae_rgb": np.mean(absolute_error, axis=0, dtype=np.float64).tolist(),
        "rmse_rgb": rmse.tolist(),
        "bias_rgb": np.mean(error, axis=0, dtype=np.float64).tolist(),
        "maximum_absolute_error_rgb": np.max(absolute_error, axis=0).tolist(),
        "absolute_error_percentiles_rgb": {
            str(percentile): np.percentile(absolute_error, percentile, axis=0).tolist()
            for percentile in (50, 90, 95, 99, 99.9)
        },
        "global_mae_rgb": float(np.mean(absolute_error, dtype=np.float64)),
        "global_rmse_rgb": global_rmse,
        "psnr_db_rgb": [
            math.inf if value == 0 else 20 * math.log10(peak / value)
            for value in rmse
        ],
        "global_psnr_db_rgb": (
            math.inf if global_rmse == 0 else 20 * math.log10(peak / global_rmse)
        ),
    }


def output_files(capture: Path) -> list[Path]:
    paths = sorted(capture.glob("output-eval-*.rgba16f.raw"))
    if not paths:
        raise ValueError(f"no output evaluation captures found under {capture}")
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_capture", type=Path)
    parser.add_argument("candidate_capture", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--psnr-peak",
        type=float,
        default=1.0,
        help="nominal RGB amplitude represented by the PSNR peak (default: 1.0)",
    )
    args = parser.parse_args()
    if args.psnr_peak <= 0:
        parser.error("--psnr-peak must be positive")

    reference_manifest = json.loads((args.reference_capture / "manifest.json").read_text())
    candidate_manifest = json.loads((args.candidate_capture / "manifest.json").read_text())
    dimensions = (
        reference_manifest["width"],
        reference_manifest["height"],
    )
    if dimensions != (candidate_manifest["width"], candidate_manifest["height"]):
        raise ValueError("capture dimensions do not match")

    reference_files = output_files(args.reference_capture)
    candidate_files = output_files(args.candidate_capture)
    if [path.name for path in reference_files] != [path.name for path in candidate_files]:
        raise ValueError("capture evaluation file sets do not match")

    result: dict[str, object] = {
        "schema": 1,
        "classification": "literal RGBA16F neural output sequence comparison",
        "reference_capture": str(args.reference_capture.resolve()),
        "candidate_capture": str(args.candidate_capture.resolve()),
        "width": dimensions[0],
        "height": dimensions[1],
        "evaluations": len(reference_files),
        "psnr_definition": {
            "peak_rgb_amplitude": args.psnr_peak,
            "formula": "20 * log10(peak_rgb_amplitude / RMSE)",
            "signal_interpretation": "nominal normalized RGB amplitude; no transfer function, primaries, or display reference assumed",
        },
        "frames": [],
        "adjacent_frame_deltas": [],
    }

    texels = dimensions[0] * dimensions[1]
    previous_reference_rgb: np.ndarray | None = None
    previous_candidate_rgb: np.ndarray | None = None
    all_errors: list[dict[str, object]] = []
    all_temporal_errors: list[dict[str, object]] = []
    for evaluation, (reference_path, candidate_path) in enumerate(
        zip(reference_files, candidate_files, strict=True), start=1
    ):
        reference_bits, reference = load_rgba16f(reference_path, texels)
        candidate_bits, candidate = load_rgba16f(candidate_path, texels)
        reference_rgb = reference[:, :3]
        candidate_rgb = candidate[:, :3]
        metrics = error_metrics(reference_rgb, candidate_rgb, args.psnr_peak)
        all_errors.append(metrics)
        differing_components = reference_bits != candidate_bits
        result["frames"].append(
            {
                "evaluation": evaluation,
                "reference_sha256": sha256(reference_path),
                "candidate_sha256": sha256(candidate_path),
                "rgba16f_byte_identical": bool(np.array_equal(reference_bits, candidate_bits)),
                "differing_rgba16f_components": int(np.count_nonzero(differing_components)),
                "differing_rgba16f_texels": int(np.count_nonzero(np.any(differing_components, axis=1))),
                "reference_distribution": distribution(reference_rgb),
                "candidate_distribution": distribution(candidate_rgb),
                "error": metrics,
            }
        )

        if previous_reference_rgb is not None and previous_candidate_rgb is not None:
            reference_delta = reference_rgb - previous_reference_rgb
            candidate_delta = candidate_rgb - previous_candidate_rgb
            temporal_error = error_metrics(reference_delta, candidate_delta, args.psnr_peak)
            all_temporal_errors.append(temporal_error)
            result["adjacent_frame_deltas"].append(
                {
                    "from_evaluation": evaluation - 1,
                    "to_evaluation": evaluation,
                    "reference_delta_mae_rgb": np.mean(
                        np.abs(reference_delta), axis=0, dtype=np.float64
                    ).tolist(),
                    "candidate_delta_mae_rgb": np.mean(
                        np.abs(candidate_delta), axis=0, dtype=np.float64
                    ).tolist(),
                    "delta_error": temporal_error,
                }
            )
        previous_reference_rgb = reference_rgb
        previous_candidate_rgb = candidate_rgb

    result["sequence_summary"] = {
        "all_frames_rgba16f_byte_identical": all(
            frame["rgba16f_byte_identical"] for frame in result["frames"]
        ),
        "mean_frame_global_mae_rgb": float(
            np.mean([metrics["global_mae_rgb"] for metrics in all_errors])
        ),
        "mean_frame_global_rmse_rgb": float(
            np.mean([metrics["global_rmse_rgb"] for metrics in all_errors])
        ),
        "mean_frame_global_psnr_db_rgb": float(
            np.mean([metrics["global_psnr_db_rgb"] for metrics in all_errors])
        ),
        "mean_adjacent_delta_global_mae_rgb": float(
            np.mean([metrics["global_mae_rgb"] for metrics in all_temporal_errors])
        ),
        "mean_adjacent_delta_global_rmse_rgb": float(
            np.mean([metrics["global_rmse_rgb"] for metrics in all_temporal_errors])
        ),
    }

    text = json.dumps(result, indent=2, allow_nan=True) + "\n"
    if args.output is None:
        print(text, end="")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(f"output: {args.output}")


if __name__ == "__main__":
    main()