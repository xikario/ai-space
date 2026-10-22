"""Explicit OCR language policy and one preflight per worker/policy."""
from functools import lru_cache
import os
from pathlib import Path
import re
import shutil
import subprocess


def languages():
    value = os.environ.get("AI_SPACE_OCR_LANGUAGES", "chi_sim+eng").strip()
    if not re.fullmatch(r"[A-Za-z0-9_]+(?:\+[A-Za-z0-9_]+)*", value):
        raise ValueError("ocr_languages_invalid")
    return value


@lru_cache(maxsize=8)
def _preflight(program, modified, policy):
    result = subprocess.run(
        [program, "--list-langs"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10
    )
    if result.returncode:
        raise RuntimeError("ocr_language_probe_failed")
    available = {
        line.strip()
        for line in result.stdout.decode("utf-8", errors="replace").splitlines()
        if re.fullmatch(r"[A-Za-z0-9_]+", line.strip())
    }
    missing = set(policy.split("+")) - available
    if missing:
        raise RuntimeError("ocr_languages_missing:" + ",".join(sorted(missing)))
    return dict(program=program, languages=policy, available=sorted(available))


def preflight():
    executable = shutil.which("tesseract")
    if not executable:
        raise RuntimeError("tesseract not installed")
    path = Path(executable).resolve()
    info = path.stat()
    return _preflight(str(path), (info.st_ino, info.st_size, info.st_mtime_ns), languages())


def command(image):
    policy = preflight()
    return [policy["program"], str(image), "stdout", "-l", policy["languages"]]
