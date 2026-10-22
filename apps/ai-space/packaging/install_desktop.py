"""Resolve the desktop entry at install time, including --prefix / DESTDIR."""
import os
from pathlib import Path
import sys
import tempfile

target, executable, icon = map(Path, sys.argv[1:])
# Exec argument quoting, followed by Desktop Entry string escaping.
argument = str(executable)
for character in ("\\", '"', "`", "$"):
    argument = argument.replace(character, "\\" + character)
argument = argument.replace("\\", "\\\\").replace("%", "%%")
icon_value = str(icon).replace("\\", "\\\\")
lines = target.read_text().splitlines()
lines = [
    ('Exec="' + argument + '" --show')
    if line.startswith("Exec=")
    else ("Icon=" + icon_value)
    if line.startswith("Icon=")
    else line
    for line in lines
]
fd, temporary = tempfile.mkstemp(prefix=".ai-space-", dir=str(target.parent))
try:
    with os.fdopen(fd, "w") as stream:
        stream.write("\n".join(lines) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o644)
    os.replace(temporary, target)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
