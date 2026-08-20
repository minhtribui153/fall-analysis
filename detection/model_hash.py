#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> tuple[str, int]:
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
            total += len(chunk)
    return digest.hexdigest(), total


def build_manifest(
    model_path: Path,
    *,
    model_id: str,
    sample_rate_hz: int,
    window_samples: int,
    slide_samples: int,
    num_axes: int,
    min_confidence: float,
) -> dict[str, Any]:
    digest, size = sha256_file(model_path)
    return {
        "model_id": model_id,
        "filename": model_path.name,
        "sha256": digest,
        "size": size,
        "sample_rate_hz": sample_rate_hz,
        "window_samples": window_samples,
        "slide_samples": slide_samples,
        "num_axes": num_axes,
        "min_confidence": min_confidence,
    }


def cmd_hash(args: argparse.Namespace) -> int:
    path = Path(args.binary)
    digest, size = sha256_file(path)
    print(digest)
    if args.verbose:
        print(f"file={path}")
        print(f"size={size}")
        print(f"sha256={digest}")
    if args.sha256_file:
        output = Path(args.sha256_file)
        output.write_text(f"{digest}  {path.name}\n", encoding="utf-8")
        print(f"wrote {output}")
    return 0


def cmd_manifest(args: argparse.Namespace) -> int:
    path = Path(args.binary)
    manifest = build_manifest(
        path,
        model_id=args.model_id,
        sample_rate_hz=args.sample_rate_hz,
        window_samples=args.window_samples,
        slide_samples=args.slide_samples,
        num_axes=args.num_axes,
        min_confidence=args.min_confidence,
    )

    payload = json.dumps(manifest, indent=2) + "\n"
    if args.output:
        output = Path(args.output)
        output.write_text(payload, encoding="utf-8")
        print(f"wrote {output}")
    else:
        print(payload, end="")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    path = Path(args.binary)
    actual, size = sha256_file(path)

    expected = args.sha256.lower().strip()
    ok = actual == expected
    print(
        json.dumps(
            {
                "file": str(path),
                "size": size,
                "expected_sha256": expected,
                "actual_sha256": actual,
                "valid": ok,
            },
            indent=2,
        )
    )
    return 0 if ok else 1


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="model-hash",
        description=(
            "Generate and verify SHA-256 integrity information for Tervanix "
            "TFLite model binaries served to ESP32Dev and Nesso N1 nodes."
        ),
    )
    sub = p.add_subparsers(dest="command", required=True)

    h = sub.add_parser("hash", help="Print SHA-256 for a model binary")
    h.add_argument("binary")
    h.add_argument("--verbose", action="store_true")
    h.add_argument(
        "--sha256-file",
        help="Also write a standard .sha256 checksum file",
    )
    h.set_defaults(func=cmd_hash)

    m = sub.add_parser("manifest", help="Generate the JSON model manifest")
    m.add_argument("binary")
    m.add_argument("--output", "-o")
    m.add_argument("--model-id", default="har-v1")
    m.add_argument("--sample-rate-hz", type=int, default=25)
    m.add_argument("--window-samples", type=int, default=50)
    m.add_argument("--slide-samples", type=int, default=25)
    m.add_argument("--num-axes", type=int, default=8)
    m.add_argument("--min-confidence", type=float, default=0.60)
    m.set_defaults(func=cmd_manifest)

    v = sub.add_parser("verify", help="Verify a binary against an expected SHA-256")
    v.add_argument("binary")
    v.add_argument("sha256")
    v.set_defaults(func=cmd_verify)

    return p


def main() -> int:
    args = parser().parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
