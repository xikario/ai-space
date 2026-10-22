"""Check or apply the pinned C++ formatter; never touches generated code."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    formatter = shutil.which("clang-format-14")
    if not formatter:
        parser.error("Install clang-format-14 for the canonical C++ style")
    root = Path(__file__).resolve().parents[1]
    names = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")
    failures = 0
    for name in names:
        if Path(name).suffix not in (".cpp", ".h"):
            continue
        path = root / name
        formatted = subprocess.check_output([formatter, "--style=file", str(path)])
        if formatted == path.read_bytes():
            continue
        if args.write:
            path.write_bytes(formatted)
        else:
            print("Formatting required:", name)
            failures += 1
    return int(bool(failures))


if __name__ == "__main__":
    sys.exit(main())
