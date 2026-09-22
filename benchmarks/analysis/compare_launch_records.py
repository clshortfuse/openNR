#!/usr/bin/env python3
"""Compare two DLSS-NR launch-record CSV files."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
import json
from pathlib import Path
from typing import Any


CONFIGURATION_FIELDS = (
    "runtime_module",
    "grid_x",
    "grid_y",
    "grid_z",
    "block_x",
    "block_y",
    "block_z",
    "dynamic_shared_memory_bytes",
    "parameter_size",
    "parameter_tail_0",
    "parameter_tail_1",
    "chain_index",
    "chain_count",
    "extended",
)
PARAMETER_FIELDS = tuple(f"parameter_qword_{index}" for index in range(10))


def load_rows(path: Path, phase: str) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    return [row for row in rows if row["phase"] == phase]


def normalized_kernel(name: str) -> str:
    return name.replace("_fp8", "")


def structural_signature(row: dict[str, str]) -> tuple[str, ...]:
    return (normalized_kernel(row["kernel"]),) + tuple(
        row[field] for field in CONFIGURATION_FIELDS
    )


def multiplicity(row: dict[str, str]) -> int:
    return int(row.get("nvapi_launch_count", "1") or "1")


def is_probable_gpu_pointer(value: int) -> bool:
    return value >= 1 << 32 and value % 256 == 0


def pointer_clusters(rows: list[dict[str, str]]) -> tuple[list[int], dict[int, str]]:
    values = sorted(
        {
            int(row[field])
            for row in rows
            for field in PARAMETER_FIELDS
            if is_probable_gpu_pointer(int(row[field]))
        }
    )
    bases: list[int] = []
    labels: dict[int, str] = {}
    previous: int | None = None
    cluster = -1
    for value in values:
        if previous is None or value - previous > 1 << 30:
            bases.append(value)
            cluster += 1
        labels[value] = f"gpu{cluster}+0x{value - bases[cluster]:X}"
        previous = value
    return bases, labels


def normalized_parameters(
    row: dict[str, str], pointer_labels: dict[int, str]
) -> tuple[str, ...]:
    result: list[str] = []
    for field in PARAMETER_FIELDS:
        value = int(row[field])
        result.append(pointer_labels.get(value, f"literal:{value}"))
    return tuple(result)


def counter_rows(counter: Counter[tuple[str, ...]], limit: int) -> list[dict[str, Any]]:
    return [
        {"count": count, "signature": list(signature)}
        for signature, count in sorted(counter.items())[:limit]
    ]


def build_counter(
    rows: list[dict[str, str]],
    signature,
) -> Counter[tuple[str, ...]]:
    result: Counter[tuple[str, ...]] = Counter()
    for row in rows:
        result[signature(row)] += multiplicity(row)
    return result


def first_difference(
    reference: list[tuple[str, ...]],
    candidate: list[tuple[str, ...]],
) -> dict[str, Any] | None:
    for index, (reference_value, candidate_value) in enumerate(
        zip(reference, candidate, strict=False), start=1
    ):
        if reference_value != candidate_value:
            return {
                "sequence": index,
                "reference": list(reference_value),
                "candidate": list(candidate_value),
            }
    if len(reference) != len(candidate):
        return {
            "sequence": min(len(reference), len(candidate)) + 1,
            "reference": None if len(reference) < len(candidate) else list(reference[-1]),
            "candidate": None if len(candidate) < len(reference) else list(candidate[-1]),
        }
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--phase", default="evaluation-001")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--difference-limit", type=int, default=40)
    args = parser.parse_args()

    reference_rows = load_rows(args.reference, args.phase)
    candidate_rows = load_rows(args.candidate, args.phase)
    reference_pointers, reference_pointer_labels = pointer_clusters(reference_rows)
    candidate_pointers, candidate_pointer_labels = pointer_clusters(candidate_rows)

    reference_structures = build_counter(reference_rows, structural_signature)
    candidate_structures = build_counter(candidate_rows, structural_signature)
    reference_topology = build_counter(
        reference_rows,
        lambda row: structural_signature(row)
        + normalized_parameters(row, reference_pointer_labels),
    )
    candidate_topology = build_counter(
        candidate_rows,
        lambda row: structural_signature(row)
        + normalized_parameters(row, candidate_pointer_labels),
    )
    reference_kernel_sequence = [
        (normalized_kernel(row["kernel"]),) for row in reference_rows
    ]
    candidate_kernel_sequence = [
        (normalized_kernel(row["kernel"]),) for row in candidate_rows
    ]
    reference_structure_sequence = [
        structural_signature(row) for row in reference_rows
    ]
    candidate_structure_sequence = [
        structural_signature(row) for row in candidate_rows
    ]
    reference_topology_sequence = [
        structural_signature(row)
        + normalized_parameters(row, reference_pointer_labels)
        for row in reference_rows
    ]
    candidate_topology_sequence = [
        structural_signature(row)
        + normalized_parameters(row, candidate_pointer_labels)
        for row in candidate_rows
    ]

    result = {
        "schema": 1,
        "phase": args.phase,
        "reference": str(args.reference.resolve()),
        "candidate": str(args.candidate.resolve()),
        "reference_rows": len(reference_rows),
        "candidate_rows": len(candidate_rows),
        "reference_launches": sum(map(multiplicity, reference_rows)),
        "candidate_launches": sum(map(multiplicity, candidate_rows)),
        "reference_pointer_cluster_bases": [f"0x{base:X}" for base in reference_pointers],
        "candidate_pointer_cluster_bases": [f"0x{base:X}" for base in candidate_pointers],
        "kernel_sequences_equal": reference_kernel_sequence == candidate_kernel_sequence,
        "structural_sequences_equal": (
            reference_structure_sequence == candidate_structure_sequence
        ),
        "topology_sequences_equal": reference_topology_sequence == candidate_topology_sequence,
        "first_kernel_sequence_difference": first_difference(
            reference_kernel_sequence, candidate_kernel_sequence
        ),
        "first_structural_sequence_difference": first_difference(
            reference_structure_sequence, candidate_structure_sequence
        ),
        "first_topology_sequence_difference": first_difference(
            reference_topology_sequence, candidate_topology_sequence
        ),
        "structural_multisets_equal": reference_structures == candidate_structures,
        "topology_multisets_equal": reference_topology == candidate_topology,
        "structural_only_in_reference": counter_rows(
            reference_structures - candidate_structures, args.difference_limit
        ),
        "structural_only_in_candidate": counter_rows(
            candidate_structures - reference_structures, args.difference_limit
        ),
        "topology_only_in_reference": counter_rows(
            reference_topology - candidate_topology, args.difference_limit
        ),
        "topology_only_in_candidate": counter_rows(
            candidate_topology - reference_topology, args.difference_limit
        ),
    }
    text = json.dumps(result, indent=2) + "\n"
    if args.output is None:
        print(text, end="")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
        print(f"output: {args.output}")


if __name__ == "__main__":
    main()