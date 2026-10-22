#!/usr/bin/env python3
"""Low-resource local knowledge base worker for UKUI Fences Smart Space.

The worker deliberately stays outside the GUI process. The index snapshot is
a file manifest; document bodies are read from originals and streamed into a
resumable SQLite/FTS5 database for
local retrieval. It never calls a remote model or sends indexed content over
the network. Every command prints one compact JSON object per line so the Qt
client and Agent Skills can consume the same protocol.
"""

import argparse
import contextlib
import fcntl
import gzip
import hashlib
import json
import os
import re
import sqlite3
import struct
import sys
import threading
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smart_space_extract import (
    Extraction,
    EXTRACTOR_VERSION,
    SUPPORTED_SUFFIXES,
    IMAGE_SUFFIXES,
    parser_version,
)

SCHEMA_VERSION = "2"
HAN_RE = re.compile(r"[\u3400-\u9fff]")
WORD_RE = re.compile(r"[0-9A-Za-z_]+|[\u3400-\u9fff]")
MAX_LEGACY_JSON_BYTES = 64 * 1024 * 1024
MAX_SEARCH_LIMIT = 1000
MAX_SEARCH_ROWS = 6000
FTS_BM25_WEIGHTS = (8.0, 3.0, 1.0)
SCORE_MIN = 65
SCORE_SPAN = 30
RELAXED_SCORE_MIN = 50
RELAXED_SCORE_SPAN = 36
RELAXED_QUERY_STOP_TOKENS = frozenset(
    "请帮我查找搜索一下有关相关资料文件内容给出列出显示所有" "这个那个哪些什么是否怎么如何的了和与或及中里内上下面" "关于按照根据"
)
EMIT_LOCK = threading.Lock()


def emit(**values):
    # Keep the JSON-lines protocol atomic for GUI and Agent Skill callers.
    with EMIT_LOCK:
        print(json.dumps(values, ensure_ascii=False, separators=(",", ":")), flush=True)


def secure_db_files(path):
    for suffix in ("", "-wal", "-shm"):
        candidate = path + suffix
        try:
            if os.path.exists(candidate):
                os.chmod(candidate, 0o600)
        except OSError:
            # Permission hardening is best-effort on non-POSIX/network
            # filesystems. SQLite will still enforce its normal access rules.
            pass


def validate_schema(db, allow_missing):
    has_meta = db.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meta'"
    ).fetchone()
    if not has_meta:
        if allow_missing:
            return
        raise RuntimeError("knowledge database schema is missing")
    existing = db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
    if not existing:
        if allow_missing:
            return
        raise RuntimeError("knowledge database schema version is missing")
    if existing[0] != SCHEMA_VERSION:
        raise RuntimeError(
            "unsupported knowledge schema version %s (expected %s)" % (existing[0], SCHEMA_VERSION)
        )


def migrate_legacy_schema(db):
    """Drop obsolete derived columns while preserving raw chunks."""
    version = db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
    if not version or version[0] != "1":
        return
    db.commit()
    db.execute("PRAGMA foreign_keys=OFF")
    db.execute("BEGIN")
    try:
        db.execute(
            """CREATE TABLE documents_clean (
                id INTEGER PRIMARY KEY,
                path TEXT NOT NULL UNIQUE,
                root TEXT NOT NULL DEFAULT '',
                name TEXT NOT NULL DEFAULT '',
                category TEXT NOT NULL DEFAULT '',
                suffix TEXT NOT NULL DEFAULT '',
                modified_ms INTEGER NOT NULL DEFAULT 0,
                size INTEGER NOT NULL DEFAULT 0,
                content_hash TEXT NOT NULL DEFAULT ''
            )"""
        )
        db.execute(
            """INSERT INTO documents_clean(
                id,path,root,name,category,suffix,modified_ms,size,content_hash)
                SELECT id,path,root,name,category,suffix,modified_ms,size,
                       content_hash
                  FROM documents"""
        )
        db.execute(
            """CREATE TABLE chunks_clean (
                id INTEGER PRIMARY KEY,
                document_id INTEGER NOT NULL REFERENCES documents_clean(id)
                    ON DELETE CASCADE,
                ordinal INTEGER NOT NULL,
                locator TEXT NOT NULL DEFAULT '',
                content TEXT NOT NULL,
                UNIQUE(document_id, ordinal)
            )"""
        )
        db.execute(
            """INSERT INTO chunks_clean(id,document_id,ordinal,locator,content)
                SELECT id,document_id,ordinal,locator,content FROM chunks"""
        )
        db.execute("DROP TABLE IF EXISTS chunks_fts")
        db.execute("DROP TABLE chunks")
        db.execute("DROP TABLE documents")
        db.execute("ALTER TABLE documents_clean RENAME TO documents")
        db.execute("ALTER TABLE chunks_clean RENAME TO chunks")
        db.execute("CREATE INDEX chunks_document_idx ON chunks(document_id)")
        db.execute(
            """CREATE VIRTUAL TABLE chunks_fts USING fts5(
                name, path, content,
                locator UNINDEXED, document_id UNINDEXED, chunk_id UNINDEXED,
                tokenize='unicode61 remove_diacritics 2')"""
        )
        for row in db.execute(
            """SELECT c.id,d.id,d.name,d.path,c.content,c.locator
                     FROM chunks c JOIN documents d ON d.id=c.document_id
                    ORDER BY c.id"""
        ):
            chunk_id, doc_id, name, path, content, locator = row
            db.execute(
                """INSERT INTO chunks_fts(
                    name,path,content,locator,document_id,chunk_id)
                    VALUES(?,?,?,?,?,?)""",
                (fts_text(name), fts_text(path), fts_text(content), locator, doc_id, chunk_id),
            )
        db.execute(
            "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version',?)", (SCHEMA_VERSION,)
        )
        db.commit()
    except Exception:
        db.rollback()
        raise
    finally:
        db.execute("PRAGMA foreign_keys=ON")


def open_db(path, read_only=False):
    path = os.path.abspath(os.path.expanduser(path))
    if read_only:
        if not os.path.isfile(path):
            raise FileNotFoundError("knowledge database does not exist")
        uri = Path(path).as_uri() + "?mode=ro"
        db = sqlite3.connect(uri, uri=True, timeout=30)
        try:
            db.execute("PRAGMA query_only=ON")
            db.execute("PRAGMA foreign_keys=ON")
            version = db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
            if version and version[0] == "1":
                db.close()
                raise ValueError("Legacy schema: migrate an explicit copy before querying")
            validate_schema(db, allow_missing=False)
            return db
        except BaseException:
            db.close()
            raise
    os.makedirs(os.path.dirname(path), exist_ok=True)
    db = sqlite3.connect(path, timeout=30)
    try:
        if db.execute("PRAGMA application_id").fetchone()[0] == 0x41494B42:
            db.close()
            raise ValueError("Imported AIKB libraries are read-only; build into a personal library")
        if os.environ.get("AI_SPACE_MAX_GIB"):
            page_size = db.execute("PRAGMA page_size").fetchone()[0]
            db.execute(
                "PRAGMA max_page_count=%d"
                % int(float(os.environ["AI_SPACE_MAX_GIB"]) * 1024 ** 3 / page_size)
            )
        db.execute("PRAGMA journal_mode=WAL")
        secure_db_files(path)
        db.execute("PRAGMA synchronous=NORMAL")
        db.execute("PRAGMA foreign_keys=ON")
        db.execute("PRAGMA temp_store=MEMORY")
        has_meta = db.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meta'"
        ).fetchone()
        if has_meta:
            version = db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
            if version and version[0] == "1":
                migrate_legacy_schema(db)
            else:
                validate_schema(db, allow_missing=True)
        db.executescript(
            """
            CREATE TABLE IF NOT EXISTS meta (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS documents (
                id INTEGER PRIMARY KEY,
                path TEXT NOT NULL UNIQUE,
                root TEXT NOT NULL DEFAULT '',
                name TEXT NOT NULL DEFAULT '',
                category TEXT NOT NULL DEFAULT '',
                suffix TEXT NOT NULL DEFAULT '',
                modified_ms INTEGER NOT NULL DEFAULT 0,
                size INTEGER NOT NULL DEFAULT 0,
                content_hash TEXT NOT NULL DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS chunks (
                id INTEGER PRIMARY KEY,
                document_id INTEGER NOT NULL REFERENCES documents(id)
                    ON DELETE CASCADE,
                ordinal INTEGER NOT NULL,
                locator TEXT NOT NULL DEFAULT '',
                content TEXT NOT NULL,
                UNIQUE(document_id, ordinal)
            );
            CREATE INDEX IF NOT EXISTS chunks_document_idx
                ON chunks(document_id);
            CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
                name, path, content,
                locator UNINDEXED, document_id UNINDEXED, chunk_id UNINDEXED,
                tokenize='unicode61 remove_diacritics 2'
            );
            CREATE TABLE IF NOT EXISTS document_availability (
                document_id INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,
                status TEXT NOT NULL DEFAULT 'present'
            );
            CREATE TABLE IF NOT EXISTS ignored_documents (
                document_id INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,
                ignored_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS seen_paths (
                path TEXT PRIMARY KEY
            );
            CREATE TABLE IF NOT EXISTS extraction_state (
                document_id INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,
                source_signature TEXT NOT NULL DEFAULT '',
                parser_version TEXT NOT NULL DEFAULT '',
                status TEXT NOT NULL DEFAULT 'legacy',
                issues_json TEXT NOT NULL DEFAULT '[]',
                source_kind TEXT NOT NULL DEFAULT '',
                indexed_chars INTEGER NOT NULL DEFAULT 0,
                extracted_blocks INTEGER NOT NULL DEFAULT 0,
                attempted_at TEXT NOT NULL DEFAULT ''
            );
            """
        )
        db.execute(
            "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version',?)", (SCHEMA_VERSION,)
        )
        db.execute(
            "INSERT OR IGNORE INTO meta(key,value) VALUES('library_id',?)",
            (str(uuid.uuid5(uuid.NAMESPACE_URL, str(Path(path).resolve()))),),
        )
        from failure_store import prepare

        prepare(db)
        has_names = db.execute(
            "SELECT 1 FROM sqlite_master WHERE name='document_names_fts'"
        ).fetchone()
        db.execute(
            "CREATE VIRTUAL TABLE IF NOT EXISTS document_names_fts USING fts5(name,document_id UNINDEXED,tokenize='unicode61 remove_diacritics 2')"
        )
        if not has_names:
            for ident, name in db.execute("SELECT id,name FROM documents"):
                db.execute(
                    "INSERT INTO document_names_fts(rowid,name,document_id) VALUES(?,?,?)",
                    (ident, " ".join(c for c in name.casefold() if c.isalnum()), ident),
                )
        align_fts_rowids(db)
        db.commit()
        secure_db_files(path)
        return db
    except BaseException:
        db.close()
        raise


def align_fts_rowids(db):
    """One-time migration enables indexed deletes rather than FTS full scans."""
    version = db.execute("SELECT value FROM meta WHERE key='fts_rowid_layout'").fetchone()
    if version and version[0] == "chunk-id-v1":
        return
    mismatch = db.execute("SELECT 1 FROM chunks_fts WHERE rowid != chunk_id LIMIT 1").fetchone()
    counts = db.execute(
        "SELECT (SELECT count(*) FROM chunks_fts),(SELECT count(*) FROM chunks)"
    ).fetchone()
    if mismatch or counts[0] != counts[1]:
        db.create_function("smart_fts_text", 1, fts_text)
        db.execute(
            """CREATE VIRTUAL TABLE chunks_fts_aligned USING fts5(
            name,path,content,locator UNINDEXED,document_id UNINDEXED,chunk_id UNINDEXED,
            tokenize='unicode61 remove_diacritics 2')"""
        )
        db.execute(
            """INSERT INTO chunks_fts_aligned(rowid,name,path,content,locator,document_id,chunk_id)
            SELECT c.id,smart_fts_text(d.name),smart_fts_text(d.path),smart_fts_text(c.content),
                   c.locator,c.document_id,c.id FROM chunks c JOIN documents d ON d.id=c.document_id"""
        )
        db.execute("DROP TABLE chunks_fts")
        db.execute("ALTER TABLE chunks_fts_aligned RENAME TO chunks_fts")
    db.execute("INSERT OR REPLACE INTO meta VALUES('fts_rowid_layout','chunk-id-v1')")


def delete_fts_document(db, doc_id):
    db.execute(
        """DELETE FROM chunks_fts WHERE rowid IN (
        SELECT id FROM chunks WHERE document_id=?)""",
        (doc_id,),
    )


def read_exact(stream, size):
    value = stream.read(size)
    if len(value) != size:
        raise EOFError("truncated index stream")
    return value


def read_u32(stream):
    return struct.unpack("<I", read_exact(stream, 4))[0]


def read_i64(stream):
    return struct.unpack("<q", read_exact(stream, 8))[0]


def read_text(stream):
    size = read_u32(stream)
    if size > 256 * 1024 * 1024:
        raise ValueError("invalid string length")
    return read_exact(stream, size).decode("utf-8", "replace")


def iter_stream(path):
    with gzip.open(path, "rb") as stream:
        if read_exact(stream, 8) != b"UKFIDX1\n":
            raise ValueError("unsupported index stream")
        version = read_u32(stream)
        if version != 1:
            raise ValueError("unsupported index version: %s" % version)
        metadata = json.loads(read_exact(stream, read_u32(stream)).decode("utf-8", "replace"))
        total = read_u32(stream)
        if metadata.get("uiStreamItems", total) != total:
            raise ValueError("Inventory item count does not match metadata")
        for _ in range(total):
            flags = read_exact(stream, 1)[0]
            size = read_i64(stream)
            modified_ms = read_i64(stream)
            values = [read_text(stream) for _ in range(8)]
            yield total, {
                "is_dir": bool(flags & 1),
                "size": size,
                "modified_ms": modified_ms,
                "path": values[0],
                "root": values[1],
                "name": values[2],
                "category": values[3],
                "content": values[4],
                "extractor": values[5],
                "ocr_status": values[6],
                "ocr_reason": values[7],
                "generated_at": metadata.get("generatedAt", ""),
            }
        if stream.read(1):
            raise ValueError("Unexpected trailing inventory data")


def iter_json(path):
    from snapshot_io import records

    generated = ""
    for key, item in records(path):
        if key == "generatedAt":
            generated = item
        if key != "item":
            continue
        yield 0, {
            "is_dir": bool(item.get("isDir")),
            "size": int(item.get("size", 0)),
            "modified_ms": int(item.get("modifiedMs", 0)),
            "path": item.get("path", ""),
            "root": item.get("root", ""),
            "name": item.get("name", ""),
            "category": item.get("category", ""),
            "content": item.get("content", ""),
            "generated_at": generated,
        }


def index_items(stream_path, json_path):
    if stream_path and os.path.isfile(stream_path):
        return iter_stream(stream_path)
    if json_path and os.path.isfile(json_path):
        return iter_json(json_path)
    raise FileNotFoundError("no readable Smart Space index snapshot")


def fts_text(value):
    """Make contiguous Chinese text queryable without a heavyweight segmenter."""
    value = value or ""
    value = HAN_RE.sub(r" \g<0> ", value)
    return re.sub(r"\s+", " ", value).strip()


def content_signature(item):
    digest = hashlib.sha256()
    digest.update(str(item["modified_ms"]).encode())
    digest.update(b"\0")
    digest.update(str(item["size"]).encode())
    digest.update(b"\0")
    digest.update((item.get("content") or "").encode("utf-8", "replace"))
    return digest.hexdigest()


def split_chunks(content, target, overlap):
    content = (content or "").replace("\x00", " ").strip()
    if not content:
        return []
    target = max(300, target)
    overlap = max(0, min(overlap, target // 3))
    chunks = []
    start = 0
    length = len(content)
    while start < length:
        ideal_end = min(length, start + target)
        end = ideal_end
        if ideal_end < length:
            floor = min(length, start + int(target * 0.65))
            candidates = [
                content.rfind(mark, floor, ideal_end)
                for mark in ("\n\n", "\n", "。", "！", "？", ". ")
            ]
            boundary = max(candidates)
            if boundary >= floor:
                end = boundary + 1
        piece = content[start:end].strip()
        if piece:
            chunks.append(piece)
        if end >= length:
            break
        start = max(start + 1, end - overlap)
    return chunks


def source_signature(item, args, ocr=None, source=None):
    try:
        if source is None:
            info = Path(item["path"]).stat()
            source = (info.st_dev, info.st_ino, info.st_mtime_ns, info.st_ctime_ns, info.st_size)
    except OSError:
        source = ("snapshot", content_signature(item))
    return hashlib.sha256(
        json.dumps(
            [
                parser_version(item["path"]),
                source,
                args.chunk_size,
                args.overlap,
                bool(args.ocr if ocr is None else ocr),
                bool(args.snapshot_only),
                "table-ranges-v2",
                os.environ.get("AI_SPACE_MAX_DOCUMENT_CHARS", "8388608"),
                os.environ.get("AI_SPACE_OCR_LANGUAGES", "chi_sim+eng")
                if (args.ocr if ocr is None else ocr)
                else "",
            ],
            sort_keys=True,
        ).encode()
    ).hexdigest()


def extraction_counts(db):
    exists = db.execute("SELECT 1 FROM sqlite_master WHERE name='extraction_state'").fetchone()
    if not exists:
        return {"legacy": db.execute("SELECT count(*) FROM documents").fetchone()[0]}
    return dict(
        db.execute(
            """SELECT coalesce(e.status,'legacy'),count(*) FROM documents d
        LEFT JOIN extraction_state e ON e.document_id=d.id GROUP BY coalesce(e.status,'legacy')"""
        )
    )


def chunks_from_blocks(blocks, name, target, overlap):
    """Coalesce small paragraphs without losing page or table-row boundaries."""
    pending = []
    start = end = ""
    length = 0

    def pieces():
        locator = start if start == end else start + " — " + end
        for part in split_chunks("\n".join(pending), target, overlap):
            yield locator, "[文档: %s | %s]\n%s" % (name, locator, part)

    for locator, content in blocks:
        if not content.strip():
            continue
        table_row = "工作表:" in locator or "表格" in locator
        table_boundary = (
            table_row
            and start
            and re.sub(r" 行\d+.*$", "", start) != re.sub(r" 行\d+.*$", "", locator)
        )
        page_boundary = start.startswith("第 ") and start.split(" | ")[0] != locator.split(" | ")[0]
        section_boundary = ("章节:" in start or "章节:" in locator) and (
            start.split(" | ")[0] != locator.split(" | ")[0]
        )
        if pending and (
            length + len(content) > target or table_boundary or page_boundary or section_boundary
        ):
            yield from pieces()
            pending, length = [], 0
        if not pending:
            start = locator
        end = locator
        pending.append(content)
        length += len(content) + 1
        if length >= target:
            yield from pieces()
            pending, length = [], 0
    if pending:
        yield from pieces()


@contextlib.contextmanager
def build_lock(path):
    path = str(Path(path).expanduser().resolve())
    os.makedirs(os.path.dirname(path), exist_ok=True)
    descriptor = os.open(path + ".build.lock", os.O_CREAT | os.O_RDWR, 0o600)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("another knowledge build is already running")
        yield
    finally:
        os.close(descriptor)


def inventory_metadata(stream_path, json_path):
    if stream_path:
        with gzip.open(stream_path, "rb") as source:
            if source.read(8) != b"UKFIDX1\n":
                raise ValueError("Invalid inventory stream")
            header = source.read(8)
            if len(header) != 8:
                raise ValueError("Truncated inventory header")
            version, length = struct.unpack("<II", header)
            if version != 1 or length > 16 * 1024 ** 2:
                raise ValueError("Unsupported inventory header")
            data = source.read(length)
            if len(data) != length:
                raise ValueError("Truncated inventory metadata")
            value = json.loads(data)
    else:
        from snapshot_io import metadata

        value = metadata(json_path)
    if stream_path and json_path and Path(json_path).is_file():
        from snapshot_io import metadata

        canonical = metadata(json_path)
        if canonical.get("generation") != value.get("generation"):
            raise ValueError("文件清单与流式索引版本不一致，请重新执行第 1 步扫描；已有知识保留")
    if value.get("errors") or value.get("stats", {}).get("truncated"):
        raise ValueError(
            "Inventory contains read failures or is incomplete; existing knowledge retained"
        )
    return value


def build(args):
    inventory_metadata(args.index_stream, args.index_json)
    from knowledge_library import storage_plan, guard_storage

    os.environ.setdefault("AI_SPACE_STORAGE_DIRECTORY", str(Path(args.db).resolve().parent))
    os.environ.setdefault("AI_SPACE_RESERVE_GIB", "2")
    os.environ.setdefault("AI_SPACE_MAX_GIB", "32")
    storage_plan(
        Path(args.db).resolve().parent,
        0,
        float(os.environ["AI_SPACE_RESERVE_GIB"]),
        float(os.environ["AI_SPACE_MAX_GIB"]),
    )
    guard_storage(
        args.db, float(os.environ["AI_SPACE_RESERVE_GIB"]), float(os.environ["AI_SPACE_MAX_GIB"])
    )
    if args.force:
        os.environ["AI_SPACE_OCR_BYPASS_CACHE"] = "1"
    with build_lock(args.db):
        build_locked(args)


def build_locked(args):
    db = open_db(args.db)
    retry_ids = set(getattr(args, "retry_document_id", []))
    if retry_ids:
        if len(retry_ids) > 200 or any(i <= 0 for i in retry_ids):
            db.close()
            raise ValueError("Select between 1 and 200 existing documents")
        ids = {
            row[0]
            for row in db.execute(
                "SELECT id FROM documents WHERE id IN (" + ",".join("?" for i in retry_ids) + ")",
                sorted(retry_ids),
            )
        }
        if not retry_ids.issubset(ids):
            db.close()
            raise ValueError("Selected retry records changed; refresh the failure list")
    from knowledge_library import ignored_ids

    skipped = ignored_ids(db)
    db.execute("DELETE FROM seen_paths")
    processed = documents = chunks_count = updated = reused = 0
    previous_failures_skipped = 0
    total = 0
    generated_at = ""
    last_progress = 0.0

    def report_extraction(count):
        unit = "blocks"
        if isinstance(count, dict):
            unit, count = count.get("unit", "blocks"), count.get("value", 0)
        nonlocal last_progress
        now = time.monotonic()
        if now - last_progress < 0.25:
            return
        last_progress = now
        emit(
            task="build",
            phase="extract",
            processed=processed,
            total=total,
            path=path,
            documents=documents,
            chunks=chunks_count,
            **{unit: count}
        )

    try:
        for total, item in index_items(args.index_stream, args.index_json):
            processed += 1
            generated_at = item.get("generated_at") or generated_at
            if item["is_dir"] or not item["path"]:
                continue
            path = os.path.abspath(item["path"])
            old = db.execute(
                "SELECT id,content_hash FROM documents WHERE path=?", (path,)
            ).fetchone()
            if retry_ids and (not old or old[0] not in retry_ids):
                continue
            if args.include_root and not any(
                path == root or path.startswith(root + os.sep) for root in args.include_root
            ):
                continue
            db.execute("INSERT OR IGNORE INTO seen_paths(path) VALUES(?)", (path,))
            suffix = Path(path).suffix.lower().lstrip(".")
            if (
                "." + suffix not in SUPPORTED_SUFFIXES
                and not item.get("content", "").strip()
                and not retry_ids
            ):
                continue
            if getattr(args, "ocr_only", False) and "." + suffix not in (IMAGE_SUFFIXES | {".pdf"}):
                continue
            if os.environ.get("AI_SPACE_STORAGE_DIRECTORY"):
                from knowledge_library import guard_storage

                guard_storage(
                    args.db,
                    float(os.environ.get("AI_SPACE_RESERVE_GIB", "2")),
                    float(os.environ.get("AI_SPACE_MAX_GIB", "32")),
                )
            source = None
            try:
                source_stat = Path(path).stat()
                source = (
                    source_stat.st_dev,
                    source_stat.st_ino,
                    source_stat.st_mtime_ns,
                    source_stat.st_ctime_ns,
                    source_stat.st_size,
                )
                item["size"] = source_stat.st_size
                item["modified_ms"] = source_stat.st_mtime_ns // 1000000
            except OSError:
                source = ("snapshot", content_signature(item))
            signature = source_signature(item, args, source=source)
            text_signature = source_signature(item, args, ocr=False, source=source)
            ocr_signature = source_signature(item, args, ocr=True, source=source)
            if old and old[0] in skipped and old[0] not in retry_ids:
                reused += 1
                documents += 1
                chunks_count += db.execute(
                    "SELECT count(*) FROM chunks WHERE document_id=?", (old[0],)
                ).fetchone()[0]
                continue
            if old and old[0] in retry_ids:
                db.execute("DELETE FROM ignored_documents WHERE document_id=?", (old[0],))
            state = (
                db.execute(
                    "SELECT source_signature,status FROM extraction_state WHERE document_id=?",
                    (old[0],),
                ).fetchone()
                if old
                else None
            )
            if (
                getattr(args, "ocr_only", False)
                and state
                and state[1] == "complete"
                and state[0] == text_signature
            ):
                reused += 1
                continue
            preserves_ocr = (
                not args.ocr
                and state
                and state[1] in {"complete", "partial", "no_text"}
                and "." + suffix in (IMAGE_SUFFIXES | {".pdf"})
                and state[0] == ocr_signature
            )
            preserves_text = (
                state
                and state[1] == "complete"
                and "." + suffix not in (IMAGE_SUFFIXES | {".pdf"})
                and state[0] in (text_signature, ocr_signature)
            )
            if (
                not args.force
                and state
                and (state[0] == signature or preserves_ocr or preserves_text)
                and (state[1] not in {"failed", "missing"} or state[0] == signature)
            ):
                reused += 1
                previous_failures_skipped += int(state[1] in {"failed", "missing"})
                documents += 1
                chunks_count += db.execute(
                    "SELECT COUNT(*) FROM chunks WHERE document_id=?", (old[0],)
                ).fetchone()[0]
            else:
                if old:
                    doc_id = old[0]
                    db.execute(
                        """UPDATE documents SET root=?,name=?,category=?,suffix=?,
                        modified_ms=?,size=? WHERE id=?""",
                        (
                            item["root"],
                            item["name"],
                            item["category"],
                            suffix,
                            item["modified_ms"],
                            item["size"],
                            doc_id,
                        ),
                    )
                else:
                    cursor = db.execute(
                        """INSERT INTO documents(
                        path,root,name,category,suffix,modified_ms,size,content_hash)
                        VALUES(?,?,?,?,?,?,?,?)""",
                        (
                            path,
                            item["root"],
                            item["name"],
                            item["category"],
                            suffix,
                            item["modified_ms"],
                            item["size"],
                            "",
                        ),
                    )
                    doc_id = cursor.lastrowid
                db.execute("DELETE FROM document_names_fts WHERE rowid=?", (doc_id,))
                db.execute(
                    "INSERT INTO document_names_fts(rowid,name,document_id) VALUES(?,?,?)",
                    (doc_id, " ".join(c for c in item["name"].casefold() if c.isalnum()), doc_id),
                )
                db.execute("SAVEPOINT extract_document")
                result = Extraction(path, ocr=args.ocr, progress=report_extraction)
                count = indexed_chars = 0
                previous_chunks = (
                    db.execute(
                        "SELECT count(*) FROM chunks WHERE document_id=?", (doc_id,)
                    ).fetchone()[0]
                    if retry_ids
                    else 0
                )
                digest = hashlib.sha256()
                try:
                    delete_fts_document(db, doc_id)
                    db.execute("DELETE FROM chunks WHERE document_id=?", (doc_id,))
                    use_snapshot = args.snapshot_only or not Path(path).is_file()
                    if "." + suffix in IMAGE_SUFFIXES and not args.ocr and item.get("content"):
                        use_snapshot = True
                    if use_snapshot:
                        if old and not Path(path).is_file() and not args.snapshot_only:
                            # Offline/removable sources must not erase good text.
                            raise FileNotFoundError(
                                "source missing; previous indexed text retained"
                            )
                        result.kind = "snapshot"
                        result.partial("preview_only:original_not_extracted")
                        blocks = [("预览快照（非完整正文）", item.get("content", ""))]
                    else:
                        blocks = result
                    for locator, piece in chunks_from_blocks(
                        blocks, item["name"], args.chunk_size, args.overlap
                    ):
                        if count % 128 == 0 and os.environ.get("AI_SPACE_STORAGE_DIRECTORY"):
                            from knowledge_library import guard_storage

                            guard_storage(
                                args.db,
                                float(os.environ.get("AI_SPACE_RESERVE_GIB", "2")),
                                float(os.environ.get("AI_SPACE_MAX_GIB", "32")),
                            )
                        cursor = db.execute(
                            "INSERT INTO chunks(document_id,ordinal,locator,content) VALUES(?,?,?,?)",
                            (doc_id, count, locator, piece),
                        )
                        db.execute(
                            """INSERT INTO chunks_fts(rowid,name,path,content,locator,document_id,chunk_id)
                            VALUES(?,?,?,?,?,?,?)""",
                            (
                                cursor.lastrowid,
                                fts_text(item["name"]),
                                fts_text(path),
                                fts_text(
                                    piece.split("\n", 1)[1]
                                    if piece.startswith("[文档: ") and "\n" in piece
                                    else piece
                                ),
                                locator,
                                doc_id,
                                cursor.lastrowid,
                            ),
                        )
                        count += 1
                        indexed_chars += len(piece)
                        digest.update(piece.encode("utf-8"))
                    if signature != source_signature(item, args):
                        raise RuntimeError("source changed during extraction; retry required")
                    if retry_ids and previous_chunks and not count:
                        raise RuntimeError(
                            "Retry produced no new text; previous indexed body retained"
                        )
                    if not count and result.status == "complete":
                        result.status = "no_text"
                    db.execute(
                        "UPDATE documents SET content_hash=? WHERE id=?",
                        (digest.hexdigest(), doc_id),
                    )
                    db.execute("RELEASE extract_document")
                    updated += 1
                except Exception as error:
                    try:
                        db.execute("ROLLBACK TO extract_document")
                        db.execute("RELEASE extract_document")
                    except sqlite3.Error:
                        db.rollback()
                        raise error  # SQLITE_FULL may already roll back the transaction.
                    from knowledge_library import StorageLimitError

                    if isinstance(error, (StorageLimitError, InterruptedError)) or (
                        isinstance(error, sqlite3.Error) and "full" in str(error).lower()
                    ):
                        raise
                    result.status = "missing" if isinstance(error, FileNotFoundError) else "failed"
                    result.issues = [
                        (str(error) or type(error).__name__)[:500],
                        "previous_chunks_retained",
                    ]
                    count, indexed_chars = db.execute(
                        "SELECT count(*),coalesce(sum(length(content)),0) FROM chunks WHERE document_id=?",
                        (doc_id,),
                    ).fetchone()
                    from knowledge_library import describe_issue

                    emit(
                        task="build",
                        phase="extraction-error",
                        path=path,
                        status=result.status,
                        error=result.issues[0],
                        errorDescription=describe_issue(result.issues[0]),
                    )
                db.execute(
                    """INSERT OR REPLACE INTO extraction_state(
                    document_id,source_signature,parser_version,status,issues_json,source_kind,
                    indexed_chars,extracted_blocks,attempted_at) VALUES(?,?,?,?,?,?,?,?,?)""",
                    (
                        doc_id,
                        signature,
                        parser_version(path),
                        result.status,
                        json.dumps(result.issues, ensure_ascii=False),
                        result.kind,
                        indexed_chars,
                        result.blocks,
                        time.strftime("%Y-%m-%dT%H:%M:%S"),
                    ),
                )
                from failure_store import store_category

                store_category(db, doc_id, result.status, result.issues)
                documents += 1
                chunks_count += count
                if retry_ids or (args.ocr and "." + suffix in (IMAGE_SUFFIXES | {".pdf"})):
                    db.commit()  # Every completed OCR document survives pause.
            if processed % 50 == 0:
                db.commit()
                emit(
                    task="build",
                    phase="build",
                    processed=processed,
                    total=total,
                    documents=documents,
                    chunks=chunks_count,
                    updated=updated,
                    reused=reused,
                )
        stale_ids = (
            []
            if retry_ids
            else [
                row[0]
                for row in db.execute(
                    "SELECT id,path FROM documents WHERE path NOT IN (SELECT path FROM seen_paths)"
                )
                if not args.include_root
                or any(
                    row[1] == root or row[1].startswith(root + os.sep) for root in args.include_root
                )
            ]
        )
        # Absence in a manifest is not proof that the user deleted a file.
        # Retain knowledge by default, including removed roots/unmounted disks.
        if retry_ids:
            for doc_id in retry_ids:
                document_path = db.execute(
                    "SELECT path FROM documents WHERE id=?", (doc_id,)
                ).fetchone()[0]
                db.execute(
                    "INSERT OR REPLACE INTO document_availability VALUES(?,?)",
                    (doc_id, "present" if Path(document_path).is_file() else "unseen"),
                )
        elif args.include_root:
            selected_roots = [os.path.abspath(root) for root in args.include_root]
            for doc_id, document_path in db.execute("SELECT id,path FROM documents"):
                absolute = os.path.abspath(document_path)
                if any(
                    absolute == root or absolute.startswith(root + os.sep)
                    for root in selected_roots
                ):
                    present = (
                        db.execute("SELECT 1 FROM seen_paths WHERE path=?", (absolute,)).fetchone()
                        is not None
                    )
                    db.execute(
                        "INSERT OR REPLACE INTO document_availability(document_id,status) VALUES(?,?)",
                        (doc_id, "present" if present else "unseen"),
                    )
        else:
            db.execute(
                "INSERT OR REPLACE INTO document_availability(document_id,status) SELECT id,CASE WHEN path IN (SELECT path FROM seen_paths) THEN 'present' ELSE 'unseen' END FROM documents"
            )
        removed_ids = (
            stale_ids if getattr(args, "prune_unseen", False) and not args.ocr_only else []
        )
        for doc_id in removed_ids:
            delete_fts_document(db, doc_id)
            db.execute("DELETE FROM document_names_fts WHERE rowid=?", (doc_id,))
            db.execute("DELETE FROM documents WHERE id=?", (doc_id,))
        # Recover derived rows left by interrupted/older builders. This never
        # deletes document text, only FTS entries without a matching raw chunk.
        repair_needed = not db.execute(
            "SELECT 1 FROM meta WHERE key='fts_orphans_checked_v1'"
        ).fetchone()
        if not retry_ids and repair_needed:
            db.execute(
                """DELETE FROM chunks_fts WHERE rowid IN (
            SELECT f.rowid FROM chunks_fts f LEFT JOIN chunks c ON c.id=f.chunk_id
            WHERE c.id IS NULL OR c.document_id != f.document_id)"""
            )
            db.execute("INSERT OR REPLACE INTO meta VALUES('fts_orphans_checked_v1','1')")
        db.execute("DELETE FROM seen_paths")
        if not args.include_root and not retry_ids:
            db.execute(
                "INSERT OR REPLACE INTO meta(key,value) VALUES('index_generated_at',?)",
                (generated_at,),
            )
        db.execute(
            "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
            (
                "last_retry_at"
                if retry_ids
                else "last_scoped_build_at"
                if args.include_root
                else "last_build_at",
                time.strftime("%Y-%m-%dT%H:%M:%S"),
            ),
        )
        if retry_ids:
            states = dict(
                db.execute(
                    "SELECT status,count(*) FROM extraction_state WHERE document_id IN ("
                    + ",".join("?" for i in retry_ids)
                    + ") GROUP BY status",
                    sorted(retry_ids),
                )
            )
        else:
            db.execute(
                "INSERT OR REPLACE INTO meta(key,value) VALUES('extractor_version',?)",
                (EXTRACTOR_VERSION,),
            )
            states = extraction_counts(db)
            db.execute(
                "INSERT OR REPLACE INTO meta(key,value) VALUES('body_source',?)",
                (
                    "preview"
                    if args.snapshot_only
                    else "mixed:original-files+legacy"
                    if states.get("legacy")
                    else "original-files",
                ),
            )
        db.commit()
        final_docs = (
            documents if retry_ids else db.execute("SELECT COUNT(*) FROM documents").fetchone()[0]
        )
        final_chunks = (
            chunks_count if retry_ids else db.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        )
        emit(
            done=True,
            task="build",
            documents=final_docs,
            chunks=final_chunks,
            updated=updated,
            reused=reused,
            previousFailuresSkipped=previous_failures_skipped,
            deleted=len(removed_ids),
            retainedAbsent=len(stale_ids) - len(removed_ids),
            extractionStates=states,
            scope="selected-documents"
            if retry_ids
            else "selected-roots"
            if args.include_root
            else "full-manifest",
            dbBytes=os.path.getsize(args.db),
        )
    finally:
        db.close()


def search(args):
    # Compatibility CLI; GUI, Skill and this entry use the same bounded core.
    from knowledge_library import search_libraries

    emit(**search_libraries(args.query, args.db, False, args.limit, 2000))


def stats(args):
    db = open_db(args.db, read_only=True)
    try:
        documents = db.execute("SELECT COUNT(*) FROM documents").fetchone()[0]
        chunks = db.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        emit(
            done=True,
            task="stats",
            documents=documents,
            chunks=chunks,
            extractionStates=extraction_counts(db),
            dbBytes=os.path.getsize(args.db) if os.path.exists(args.db) else 0,
            lastBuild=dict(
                db.execute("SELECT key,value FROM meta WHERE key='last_build_at'").fetchall()
            ).get("last_build_at", ""),
        )
    finally:
        db.close()


def issues(args):
    from knowledge_library import issue_fields

    db = open_db(args.db, read_only=True)
    try:
        exists = db.execute("SELECT 1 FROM sqlite_master WHERE name='extraction_state'").fetchone()
        rows = (
            db.execute(
                """SELECT d.path,e.status,e.issues_json,e.source_kind,e.indexed_chars
            FROM extraction_state e JOIN documents d ON d.id=e.document_id
            WHERE e.status NOT IN ('complete','no_text') ORDER BY d.path LIMIT ?""",
                (max(1, min(args.limit, 10000)),),
            ).fetchall()
            if exists
            else []
        )
        emit(
            done=True,
            task="issues",
            extractionStates=extraction_counts(db),
            results=[
                dict(
                    path=p,
                    status=s,
                    issues=json.loads(j),
                    extractor=k,
                    indexedChars=n,
                    **issue_fields(json.loads(j))
                )
                for p, s, j, k, n in rows
            ],
        )
    finally:
        db.close()


def parser():
    root = argparse.ArgumentParser()
    commands = root.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build")
    build_parser.add_argument("--index-stream", default="")
    build_parser.add_argument("--index-json", default="")
    build_parser.add_argument("--db", required=True)
    build_parser.add_argument("--chunk-size", type=int, default=1200)
    build_parser.add_argument("--overlap", type=int, default=120)
    build_parser.add_argument(
        "--ocr",
        action="store_true",
        default=False,
        help="explicitly enable OCR (default is text-only)",
    )
    build_parser.add_argument(
        "--no-ocr",
        dest="ocr",
        action="store_false",
        help="text-only pass; missing OCR is explicitly partial",
    )
    build_parser.add_argument(
        "--ocr-only", action="store_true", help="only pending images/PDF; preserve text documents"
    )
    build_parser.add_argument(
        "--prune-unseen",
        action="store_true",
        help="explicitly delete documents absent from the complete manifest; default retains their text",
    )
    build_parser.add_argument("--force", action="store_true", help="ignore extraction cache")
    build_parser.add_argument(
        "--retry-document-id",
        action="append",
        type=int,
        default=[],
        help="explicitly retry these existing documents; never prune or alter other availability flags",
    )
    build_parser.add_argument(
        "--snapshot-only", action="store_true", help="compatibility import; always marked partial"
    )
    build_parser.add_argument(
        "--include-root",
        action="append",
        default=[],
        type=lambda value: os.path.abspath(os.path.expanduser(value)),
        help="refresh selected manifest subtree without pruning other roots",
    )
    build_parser.set_defaults(handler=build)
    search_child = commands.add_parser("search")
    search_child.add_argument("--db", required=True)
    search_child.add_argument("--query", required=True)
    search_child.add_argument("--limit", type=int, default=120)
    search_child.set_defaults(handler=search)
    stats_parser = commands.add_parser("stats")
    stats_parser.add_argument("--db", required=True)
    stats_parser.set_defaults(handler=stats)
    issues_parser = commands.add_parser("issues")
    issues_parser.add_argument("--db", required=True)
    issues_parser.add_argument("--limit", type=int, default=100)
    issues_parser.set_defaults(handler=issues)
    return root


def main():
    args = parser().parse_args()
    if args.command == "build":
        os.environ.setdefault("OMP_THREAD_LIMIT", "2")
        try:
            os.nice(15)
        except (AttributeError, OSError):
            pass
    try:
        args.handler(args)
    except Exception as error:
        emit(
            done=True,
            task=args.command,
            error=str(error)[:500],
            paused=isinstance(error, InterruptedError),
            errorType=type(error).__name__,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
