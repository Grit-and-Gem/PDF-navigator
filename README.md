# PDF-navigator
Building Tool to navigate the PDF-research papers efficiently

## Florence-2 Pipeline (Ollama)

Run image understanding over converted PDF pages using Ollama + Florence-2.

### 1) Ensure model is available

```bash
ollama pull florence2
```

### 2) Run pipeline

```bash
python3 pipeline/florence2_pipeline.py --model florence2
```

### 3) Useful options

```bash
# Custom prompt
python3 pipeline/florence2_pipeline.py --model florence2 --prompt "Summarize this page in 5 bullet points"

# Process only one image/<unique_code> folder
python3 pipeline/florence2_pipeline.py --model florence2 --only-unique-code 18a1b26678b7ccfd

# Repoint paths and limit pages
python3 pipeline/florence2_pipeline.py --model florence2 --image-root image --mapping config/pdf_image_map.json --limit 5
```

Output is written to `output/florence2_results.json` by default.
