#!/usr/bin/env python3
import json
import secrets
import shutil
import subprocess
import sys
import time
from pathlib import Path


def make_unique_code() -> str:
    # 16 hex chars from time + randomness to reduce collision chance.
    time_part = f"{time.time_ns():x}"
    random_part = secrets.token_hex(8)
    return (time_part + random_part)[:16]


def load_mapping(mapping_file: Path) -> dict:
    if not mapping_file.exists():
        return {}

    try:
        data = json.loads(mapping_file.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}

    if isinstance(data, dict):
        return data
    return {}


def save_mapping(mapping_file: Path, mapping: dict) -> None:
    mapping_file.parent.mkdir(parents=True, exist_ok=True)
    mapping_file.write_text(json.dumps(mapping, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    root = Path.cwd()
    pdf_dir = root / "pdf"
    image_dir = root / "image"
    mapping_file = root / "config" / "pdf_image_map.json"

    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <pdf-file-name> [unique-code]", file=sys.stderr)
        print(f"Example: {sys.argv[0]} paper.pdf", file=sys.stderr)
        return 1

    pdf_file_name = sys.argv[1]
    pdf_path = pdf_dir / pdf_file_name

    if not pdf_path.exists():
        print(f"PDF file not found: {pdf_path}", file=sys.stderr)
        return 1

    if shutil.which("mutool") is None:
        print("Conversion failed. Ensure mutool is installed (mupdf-tools).", file=sys.stderr)
        return 1

    unique_code = sys.argv[2] if len(sys.argv) >= 3 else make_unique_code()
    output_folder = image_dir / unique_code
    output_folder.mkdir(parents=True, exist_ok=True)

    output_pattern = output_folder / "page-%d.png"
    command = [
        "mutool",
        "draw",
        "-q",
        "-F",
        "png",
        "-o",
        str(output_pattern),
        str(pdf_path),
    ]

    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        print("Conversion failed while running mutool draw.", file=sys.stderr)
        return result.returncode

    mapping = load_mapping(mapping_file)
    mapping[pdf_file_name] = unique_code
    save_mapping(mapping_file, mapping)

    print(f"Converted: {pdf_path}")
    print(f"Images saved in: {output_folder}")
    print(f"Mapping updated in: {mapping_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
