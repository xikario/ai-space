"""Leased task scratch space; recover interrupted jobs without touching bodies."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import tempfile

from text_safety import open_regular_binary

MAGIC = "ai-space-job-session-v1"


def clean_interrupted(directory):
    directory = Path(directory)
    removed = 0
    retained = 0
    for path in directory.iterdir():
        if not path.name.startswith("job-") or path.is_symlink() or not path.is_dir():
            continue
        marker = path / ".ai-space-session"
        try:
            if marker.is_symlink():
                continue
            with open_regular_binary(marker) as owner:
                try:
                    fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    continue
                data = owner.read(4096)
                if not data.startswith((MAGIC + "\n").encode()):
                    continue
                shutil.rmtree(path)
                removed += 1
        except FileNotFoundError:
            continue
        except (OSError, ValueError, RuntimeError):
            retained += 1
    return removed, retained


class JobSession:
    def __init__(self, directory):
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        self.recovered, self.retained = clean_interrupted(directory)
        self.temporary = tempfile.TemporaryDirectory(prefix="job-", dir=str(directory))
        self.name = self.temporary.name
        self.owner = (Path(self.name) / ".ai-space-session").open("x+b")
        fcntl.flock(self.owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.stage("preparing")

    def fileno(self):
        return self.owner.fileno()

    def stage(self, name):
        self.owner.seek(0)
        self.owner.write((MAGIC + "\n" + json.dumps(dict(stage=name))).encode("utf-8"))
        self.owner.truncate()
        self.owner.flush()
        os.fsync(self.owner.fileno())

    def cleanup(self):
        # Never mask a job's final JSON result with an exception from removal.
        # An unremovable marked session remains eligible for a later recovery.
        try:
            self.temporary.cleanup()
        except OSError:
            pass
        finally:
            self.owner.close()
