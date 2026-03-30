#!/usr/bin/env python3
"""
Process converted PDF page images with Ollama (Florence-2 or any vision-capable model).

Default behavior:
- Scans image/<unique_code>/*.png
- Sends each image to Ollama /api/generate
- Writes structured output to output/florence2_results.json

Example:
  python3 pipeline/florence2_pipeline.py --model florence2 --prompt "Summarize this page"
"""

import argparse
import base64
import json
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass
class PageJob:
    unique_code: str
    image_path: Path
    page_number: int
    pdf_name: Optional[str] = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Florence-2 image processing through Ollama.")
    parser.add_argument("--model", default="florence2", help="Ollama model name (default: florence2)")
    parser.add_argument(
        "--prompt",
        default=(
            "Analyze this PDF page and return concise JSON with keys: "
            "title_guess, summary, key_points, extracted_text_snippet."
        ),
        help="Prompt sent for each image",
    )
    parser.add_argument("--image-root", default="image", help="Root image directory (default: image)")
    parser.add_argument("--mapping", default="config/pdf_image_map.json", help="PDF-to-unique-id mapping file")
    parser.add_argument("--output", default="output/florence2_results.json", help="Output JSON file path")
    parser.add_argument("--host", default="http://127.0.0.1:11434", help="Ollama host URL")
    parser.add_argument("--temperature", type=float, default=0.1, help="Sampling temperature")
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Limit number of pages to process (0 = all)",
    )
    parser.add_argument(
        "--only-unique-code",
        default="",
        help="Process only one unique code folder, e.g. 18a1b26678b7ccfd",
    )
    parser.add_argument(
        "--pull-if-missing",
        action="store_true",
        help="Attempt `ollama pull <model>` if model is not available",
    )
    return parser.parse_args()


def parse_mapping(mapping_path: Path) -> Dict[str, str]:
    if not mapping_path.exists():
        return {}

    raw = mapping_path.read_text(encoding="utf-8", errors="ignore")

    try:
        data = json.loads(raw)
        if isinstance(data, dict):
            return {str(k): str(v) for k, v in data.items()}
    except json.JSONDecodeError:
        pass

    # Fallback for malformed JSON: capture "pdf_name": "unique_code" pairs.
    pairs = re.findall(r'"([^"\\]+)"\s*:\s*"([^"\\]+)"', raw)
    return {k: v for k, v in pairs}


def reverse_mapping(pdf_to_code: Dict[str, str]) -> Dict[str, str]:
    code_to_pdf: Dict[str, str] = {}
    for pdf_name, unique_code in pdf_to_code.items():
        code_to_pdf[unique_code] = pdf_name
    return code_to_pdf


def page_number_from_name(image_path: Path) -> int:
    match = re.search(r"(\\d+)", image_path.stem)
    if match:
        return int(match.group(1))
    return 10**9


def discover_jobs(image_root: Path, code_to_pdf: Dict[str, str], only_unique_code: str) -> List[PageJob]:
    if not image_root.exists():
        return []

    jobs: List[PageJob] = []
    for code_dir in sorted([p for p in image_root.iterdir() if p.is_dir()], key=lambda p: p.name):
        unique_code = code_dir.name
        if only_unique_code and unique_code != only_unique_code:
            continue

        pngs = sorted(
            [p for p in code_dir.glob("*.png") if p.is_file()],
            key=page_number_from_name,
        )
        for img in pngs:
            jobs.append(
                PageJob(
                    unique_code=unique_code,
                    image_path=img,
                    page_number=page_number_from_name(img),
                    pdf_name=code_to_pdf.get(unique_code),
                )
            )
    return jobs


def ensure_ollama_available(model: str, pull_if_missing: bool) -> None:
    if shutil.which("ollama") is None:
        raise RuntimeError("`ollama` command not found. Install Ollama first.")

    tags_cmd = ["ollama", "list"]
    result = subprocess.run(tags_cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError("Failed to query Ollama models. Is Ollama running?")

    if model in result.stdout:
        return

    if not pull_if_missing:
        raise RuntimeError(
            f"Model '{model}' is not available locally. Run `ollama pull {model}` or pass --pull-if-missing."
        )

    pull_cmd = ["ollama", "pull", model]
    pull_result = subprocess.run(pull_cmd, check=False)
    if pull_result.returncode != 0:
        raise RuntimeError(f"Failed to pull model '{model}'.")


def ollama_generate(host: str, model: str, prompt: str, image_bytes: bytes, temperature: float) -> Dict:
    payload = {
        "model": model,
        "prompt": prompt,
        "stream": False,
        "images": [base64.b64encode(image_bytes).decode("ascii")],
        "options": {"temperature": temperature},
    }

    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url=f"{host.rstrip('/')}/api/generate",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=180) as response:
            body = response.read().decode("utf-8")
            return json.loads(body)
    except urllib.error.HTTPError as err:
        detail = err.read().decode("utf-8", errors="ignore")
        raise RuntimeError(f"Ollama HTTP error {err.code}: {detail}") from err
    except urllib.error.URLError as err:
        raise RuntimeError(f"Cannot connect to Ollama at {host}: {err}") from err


def run_pipeline(args: argparse.Namespace) -> int:
    image_root = Path(args.image_root)
    mapping_path = Path(args.mapping)
    output_path = Path(args.output)

    ensure_ollama_available(args.model, args.pull_if_missing)

    mapping = parse_mapping(mapping_path)
    code_to_pdf = reverse_mapping(mapping)
    jobs = discover_jobs(image_root, code_to_pdf, args.only_unique_code)

    if args.limit > 0:
        jobs = jobs[: args.limit]

    if not jobs:
        print("No page images found to process.")
        return 0

    output_path.parent.mkdir(parents=True, exist_ok=True)

    results = {
        "model": args.model,
        "prompt": args.prompt,
        "image_root": str(image_root),
        "total_pages": len(jobs),
        "items": [],
    }

    for idx, job in enumerate(jobs, start=1):
        print(f"[{idx}/{len(jobs)}] Processing {job.image_path}")
        image_bytes = job.image_path.read_bytes()
        response = ollama_generate(
            host=args.host,
            model=args.model,
            prompt=args.prompt,
            image_bytes=image_bytes,
            temperature=args.temperature,
        )

        item = {
            "unique_code": job.unique_code,
            "pdf_name": job.pdf_name,
            "page_number": job.page_number,
            "image_path": str(job.image_path),
            "response": response.get("response", ""),
            "done": response.get("done", True),
            "eval_count": response.get("eval_count"),
            "eval_duration": response.get("eval_duration"),
        }
        results["items"].append(item)

    output_path.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"Saved results to: {output_path}")
    return 0


def main() -> int:
    args = parse_args()
    try:
        return run_pipeline(args)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
