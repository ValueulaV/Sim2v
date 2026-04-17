"""Shared utilities: logging, code extraction, and wrapper header parsing."""

import os
import re
import logging
from datetime import datetime


def setup_logger(name, log_dir, level=logging.DEBUG):
    """Setup logger with file + console handlers. File logs everything, console logs INFO+."""
    os.makedirs(log_dir, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    base = os.path.join(log_dir, f"{name}_{ts}.log")
    log_file = base
    if os.path.exists(log_file):
        for i in range(1, 1000):
            cand = os.path.join(log_dir, f"{name}_{ts}_{i:03d}.log")
            if not os.path.exists(cand):
                log_file = cand
                break

    logger = logging.getLogger(name)
    logger.setLevel(level)
    logger.handlers.clear()

    fh = logging.FileHandler(log_file, encoding="utf-8")
    fh.setLevel(level)
    fh.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
    logger.addHandler(fh)

    ch = logging.StreamHandler()
    ch.setLevel(logging.INFO)
    ch.setFormatter(logging.Formatter("[%(levelname)s] %(message)s"))
    logger.addHandler(ch)

    return logger


def extract_verilog(text):
    """Extract verilog code block from LLM response."""
    raw = text or ""
    for tag in ["systemverilog", "verilog", "sv", ""]:
        # Normal fenced block (with closing ```), allowing optional newline.
        pat = rf"```{tag}\s*\n?(.*?)```"
        matches = re.findall(pat, raw, re.DOTALL | re.IGNORECASE)
        if matches:
            return matches[-1].strip()

        # Fallback: unclosed fenced block until end-of-text.
        open_pat = rf"```{tag}\s*\n?(.*)$"
        m = re.search(open_pat, raw, re.DOTALL | re.IGNORECASE)
        if m:
            return m.group(1).strip()
    return ""


def extract_answer(text):
    """Extract content inside <answer>...</answer>.

    If tags do not exist, fall back to the original full text.
    """
    m = re.search(r"<answer>\s*(.*?)\s*</answer>", text, re.DOTALL | re.IGNORECASE)
    if m:
        return m.group(1).strip()
    return text.strip()


def extract_model_payload(text, use_think):
    """Extract effective model payload based on prompt protocol.

    - use_think=True: parse <answer>...</answer>, fallback to full text if tags missing.
    - use_think=False: use full raw text directly.
    """
    if use_think:
        return extract_answer(text)
    return (text or "").strip()


def parse_cpp_header(path):
    """Parse PI_WIDTH, PO_WIDTH and function body from cpp header file."""
    with open(path) as f:
        content = f.read()

    pi_width = int(re.search(r"PI_WIDTH\s*=\s*(\d+)", content).group(1))
    po_width = int(re.search(r"PO_WIDTH\s*=\s*(\d+)", content).group(1))

    func_match = re.search(
        r"void\s+io_generator_outer\s*\([^)]*\)\s*\{(.*)\}",
        content,
        re.DOTALL,
    )
    body = func_match.group(1).strip()

    return {"pi_width": pi_width, "po_width": po_width, "body": body, "full": content}
