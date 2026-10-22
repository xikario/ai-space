"""Reproducible syntax, lint and formatting checks for owned Python sources."""
import argparse
import ast
from pathlib import Path
import sys

import black
from pyflakes.api import checkPath
from pyflakes.reporter import Reporter


def sources(root):
    app = root / "apps" / "ai-space"
    paths = list((app / "scripts").glob("*.py"))
    paths += list((app / "tests").glob("*.py"))
    paths += list((app / "packaging").glob("*.py"))
    paths += list((app / "skills").rglob("*.py"))
    paths += list((root / "tools").glob("*.py"))
    return sorted(paths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    if black.__version__ != "19.3b0":
        parser.error("Install the pinned tools from requirements-dev.txt")
    root = Path(__file__).resolve().parents[1]
    errors = 0
    reporter = Reporter(sys.stdout, sys.stderr)
    for path in sources(root):
        text = path.read_text(encoding="utf-8")
        ast.parse(text, filename=str(path), feature_version=8)
        try:
            formatted = black.format_file_contents(
                text, fast=False, mode=black.FileMode(line_length=100)
            )
        except black.NothingChanged:
            formatted = text
        if formatted != text:
            if args.write:
                path.write_text(formatted, encoding="utf-8")
            else:
                print("Formatting required:", path.relative_to(root))
                errors += 1
        # Vendored dependencies are excluded. Fixtures have intentionally unused
        # variables and imports; production and developer tools must be clean.
        if path.parent in (app_scripts(root), root / "tools"):
            errors += checkPath(str(path), reporter)
    print("Python 3.8 syntax, owned-source lint and formatting checked")
    return int(bool(errors))


def app_scripts(root):
    return root / "apps" / "ai-space" / "scripts"


if __name__ == "__main__":
    sys.exit(main())
