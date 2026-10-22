"""Bounded page cache with an indexed size ledger, not a directory scan per page."""
import contextlib
import fcntl
import stat
import os
from pathlib import Path
import re
import sqlite3
import tempfile
import time

_CACHES = {}
MAX_PAGES = 20000
NAME = re.compile(r"[a-f0-9]{64}\.txt")
TEMP_NAME = re.compile(r"\.page-[a-z0-9_]{8}")


class PageCache:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.folder.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.db = sqlite3.connect(str(self.folder / ".pages.sqlite"), timeout=0.2)
        self.db.executescript(
            """CREATE TABLE IF NOT EXISTS pages(
            name TEXT PRIMARY KEY,size INTEGER NOT NULL,modified INTEGER NOT NULL);
            CREATE INDEX IF NOT EXISTS pages_oldest ON pages(modified,name);
            CREATE TABLE IF NOT EXISTS counters(id INTEGER PRIMARY KEY,total INTEGER NOT NULL,entries INTEGER NOT NULL DEFAULT 0);"""
        )
        if "entries" not in {row[1] for row in self.db.execute("PRAGMA table_info(counters)")}:
            self.db.execute("ALTER TABLE counters ADD COLUMN entries INTEGER NOT NULL DEFAULT 0")
        # Reconcile once per worker, including a crash between rename and ledger commit.
        self.db.execute("DELETE FROM pages")
        total = count = 0
        with os.scandir(self.folder) as entries:
            for entry in entries:
                if TEMP_NAME.fullmatch(entry.name):
                    self.remove_abandoned(entry)
                if NAME.fullmatch(entry.name) and entry.is_file(follow_symlinks=False):
                    info = entry.stat(follow_symlinks=False)
                    self.db.execute(
                        "INSERT INTO pages VALUES(?,?,?)",
                        (entry.name, info.st_size, info.st_mtime_ns),
                    )
                    total += info.st_size
                    count += 1
        self.db.execute("INSERT OR REPLACE INTO counters VALUES(1,?,?)", (total, count))
        self.db.commit()

    def remove_abandoned(self, entry):
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
        try:
            descriptor = os.open(entry.path, flags)
        except OSError:
            return
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
                return
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return
            current = os.lstat(entry.path)
            if (current.st_dev, current.st_ino) == (info.st_dev, info.st_ino):
                os.unlink(entry.path)
        except OSError:
            pass
        finally:
            os.close(descriptor)

    def save(self, target, text, limit):
        data = text.encode("utf-8")
        descriptor, temporary = tempfile.mkstemp(prefix=".page-", dir=str(self.folder))
        try:
            # Hold the same inode lease until rename/cleanup, including while
            # waiting for the ledger transaction. Other workers retain it.
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            with os.fdopen(os.dup(descriptor), "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            self.db.execute("BEGIN IMMEDIATE")
            old = self.db.execute("SELECT size FROM pages WHERE name=?", (target.name,)).fetchone()
            total, count = self.db.execute(
                "SELECT total,entries FROM counters WHERE id=1"
            ).fetchone()
            total -= old[0] if old else 0
            count -= int(old is not None)
            self.db.execute("DELETE FROM pages WHERE name=?", (target.name,))
            while total + len(data) > limit or count >= MAX_PAGES:
                row = self.db.execute(
                    "SELECT name,size FROM pages ORDER BY modified,name LIMIT 1"
                ).fetchone()
                if not row:
                    break
                with contextlib.suppress(FileNotFoundError):
                    (self.folder / row[0]).unlink()
                self.db.execute("DELETE FROM pages WHERE name=?", (row[0],))
                total -= row[1]
                count -= 1
            os.replace(temporary, target)
            self.db.execute(
                "INSERT INTO pages VALUES(?,?,?)", (target.name, len(data), time.time_ns())
            )
            self.db.execute(
                "UPDATE counters SET total=?,entries=? WHERE id=1", (total + len(data), count + 1)
            )
            self.db.commit()
        except BaseException:
            self.db.rollback()
            raise
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temporary)
            os.close(descriptor)


def save(target, text, limit):
    folder = str(target.parent.resolve())
    cache = _CACHES.get(folder)
    if cache is None:
        cache = _CACHES[folder] = PageCache(folder)
    cache.save(target, text, limit)
