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
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smart_space_extract import Extraction, EXTRACTOR_VERSION, SUPPORTED_SUFFIXES, IMAGE_SUFFIXES

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
    "请帮我查找搜索一下有关相关资料文件内容给出列出显示所有"
    "这个那个哪些什么是否怎么如何的了和与或及中里内上下面"
    "关于按照根据")
EMIT_LOCK = threading.Lock()


def emit(**values):
    # Keep the JSON-lines protocol atomic for GUI and Agent Skill callers.
    with EMIT_LOCK:
        print(json.dumps(values, ensure_ascii=False, separators=(",", ":")),
              flush=True)


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
    existing = db.execute(
        "SELECT value FROM meta WHERE key='schema_version'"
    ).fetchone()
    if not existing:
        if allow_missing:
            return
        raise RuntimeError("knowledge database schema version is missing")
    if existing[0] != SCHEMA_VERSION:
        raise RuntimeError(
            "unsupported knowledge schema version %s (expected %s)"
            % (existing[0], SCHEMA_VERSION))


def migrate_legacy_schema(db):
    """Drop obsolete derived columns while preserving raw chunks."""
    version = db.execute(
        "SELECT value FROM meta WHERE key='schema_version'"
    ).fetchone()
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
                    ORDER BY c.id""").fetchall():
            chunk_id, doc_id, name, path, content, locator = row
            db.execute(
                """INSERT INTO chunks_fts(
                    name,path,content,locator,document_id,chunk_id)
                    VALUES(?,?,?,?,?,?)""",
                (fts_text(name), fts_text(path), fts_text(content),
                 locator, doc_id, chunk_id))
        db.execute(
            "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version',?)",
            (SCHEMA_VERSION,))
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
        db.execute("PRAGMA query_only=ON")
        db.execute("PRAGMA foreign_keys=ON")
        version = db.execute(
            "SELECT value FROM meta WHERE key='schema_version'"
        ).fetchone()
        if version and version[0] == "1":
            db.close()
            migrated = open_db(path, read_only=False)
            migrated.close()
            return open_db(path, read_only=True)
        validate_schema(db, allow_missing=False)
        return db
    os.makedirs(os.path.dirname(path), exist_ok=True)
    db = sqlite3.connect(path, timeout=30)
    db.execute("PRAGMA journal_mode=WAL")
    secure_db_files(path)
    db.execute("PRAGMA synchronous=NORMAL")
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA temp_store=MEMORY")
    has_meta = db.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meta'"
    ).fetchone()
    if has_meta:
        version = db.execute(
            "SELECT value FROM meta WHERE key='schema_version'"
        ).fetchone()
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
    db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version',?)",
               (SCHEMA_VERSION,))
    align_fts_rowids(db)
    db.commit()
    secure_db_files(path)
    return db


def align_fts_rowids(db):
    """One-time migration enables indexed deletes rather than FTS full scans."""
    version = db.execute("SELECT value FROM meta WHERE key='fts_rowid_layout'").fetchone()
    if version and version[0] == 'chunk-id-v1':
        return
    mismatch = db.execute("SELECT 1 FROM chunks_fts WHERE rowid != chunk_id LIMIT 1").fetchone()
    counts = db.execute("SELECT (SELECT count(*) FROM chunks_fts),(SELECT count(*) FROM chunks)").fetchone()
    if mismatch or counts[0] != counts[1]:
        db.create_function('smart_fts_text', 1, fts_text)
        db.execute('''CREATE VIRTUAL TABLE chunks_fts_aligned USING fts5(
            name,path,content,locator UNINDEXED,document_id UNINDEXED,chunk_id UNINDEXED,
            tokenize='unicode61 remove_diacritics 2')''')
        db.execute('''INSERT INTO chunks_fts_aligned(rowid,name,path,content,locator,document_id,chunk_id)
            SELECT c.id,smart_fts_text(d.name),smart_fts_text(d.path),smart_fts_text(c.content),
                   c.locator,c.document_id,c.id FROM chunks c JOIN documents d ON d.id=c.document_id''')
        db.execute('DROP TABLE chunks_fts')
        db.execute('ALTER TABLE chunks_fts_aligned RENAME TO chunks_fts')
    db.execute("INSERT OR REPLACE INTO meta VALUES('fts_rowid_layout','chunk-id-v1')")


def delete_fts_document(db, doc_id):
    db.execute('''DELETE FROM chunks_fts WHERE rowid IN (
        SELECT id FROM chunks WHERE document_id=?)''', (doc_id,))


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
        metadata = json.loads(read_exact(stream, read_u32(stream))
                              .decode("utf-8", "replace"))
        total = read_u32(stream)
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


def iter_json(path):
    size = os.path.getsize(path)
    if size > MAX_LEGACY_JSON_BYTES:
        raise ValueError(
            "legacy JSON index is too large (%d MB); use the .ui.bin.gz "
            "stream index instead" % (size // 1024 // 1024))
    with open(path, "r", encoding="utf-8") as stream:
        root = json.load(stream)
    items = root.get("items", [])
    total = len(items)
    for item in items:
        yield total, {
            "is_dir": bool(item.get("isDir")),
            "size": int(item.get("size", 0)),
            "modified_ms": int(item.get("modifiedMs", 0)),
            "path": item.get("path", ""),
            "root": item.get("root", ""),
            "name": item.get("name", ""),
            "category": item.get("category", ""),
            "content": item.get("content", ""),
            "generated_at": root.get("generatedAt", ""),
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
            candidates = [content.rfind(mark, floor, ideal_end)
                          for mark in ("\n\n", "\n", "。", "！", "？", ". ")]
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


def source_signature(item, args, ocr=None):
    try:
        info = Path(item["path"]).stat()
        source = (info.st_mtime_ns, info.st_size)
    except OSError:
        source = ("snapshot", content_signature(item))
    return hashlib.sha256(json.dumps([
        EXTRACTOR_VERSION, source, args.chunk_size, args.overlap,
        bool(args.ocr if ocr is None else ocr), bool(args.snapshot_only)
    ], sort_keys=True).encode()).hexdigest()


def extraction_counts(db):
    exists = db.execute("SELECT 1 FROM sqlite_master WHERE name='extraction_state'").fetchone()
    if not exists:
        return {"legacy": db.execute("SELECT count(*) FROM documents").fetchone()[0]}
    return dict(db.execute("""SELECT coalesce(e.status,'legacy'),count(*) FROM documents d
        LEFT JOIN extraction_state e ON e.document_id=d.id GROUP BY coalesce(e.status,'legacy')"""))


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
        page_boundary = start.startswith("第 ") and start.split(" | ")[0] != locator.split(" | ")[0]
        section_boundary = ("章节:" in start or "章节:" in locator) and (
            start.split(" | ")[0] != locator.split(" | ")[0])
        if pending and (length + len(content) > target or table_row or page_boundary or section_boundary):
            yield from pieces()
            pending, length = [], 0
        if not pending:
            start = locator
        end = locator
        pending.append(content)
        length += len(content) + 1
        if length >= target or table_row:
            yield from pieces()
            pending, length = [], 0
    if pending:
        yield from pieces()


@contextlib.contextmanager
def build_lock(path):
    path = os.path.abspath(os.path.expanduser(path))
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


def build(args):
    with build_lock(args.db):
        build_locked(args)


def build_locked(args):
    db = open_db(args.db)
    db.execute("DELETE FROM seen_paths")
    processed = documents = chunks_count = updated = reused = 0
    total = 0
    generated_at = ""
    try:
        for total, item in index_items(args.index_stream, args.index_json):
            processed += 1
            generated_at = item.get("generated_at") or generated_at
            if item["is_dir"] or not item["path"]:
                continue
            path = os.path.abspath(item["path"])
            if args.include_root and not any(path == root or path.startswith(root + os.sep)
                                             for root in args.include_root):
                continue
            db.execute("INSERT OR IGNORE INTO seen_paths(path) VALUES(?)", (path,))
            suffix = Path(path).suffix.lower().lstrip(".")
            if "." + suffix not in SUPPORTED_SUFFIXES and not item.get("content", "").strip():
                continue
            signature = source_signature(item, args)
            try:
                source_stat = Path(path).stat()
                item["size"] = source_stat.st_size
                item["modified_ms"] = source_stat.st_mtime_ns // 1000000
            except OSError:
                pass
            old = db.execute("SELECT id,content_hash FROM documents WHERE path=?", (path,)).fetchone()
            state = db.execute("SELECT source_signature,status FROM extraction_state WHERE document_id=?",
                               (old[0],)).fetchone() if old else None
            preserves_ocr = (not args.ocr and state and state[1] == "complete"
                and "." + suffix in (IMAGE_SUFFIXES | {".pdf"})
                and state[0] == source_signature(item, args, ocr=True))
            if (not args.force and state and (state[0] == signature or preserves_ocr)
                    and state[1] not in {"failed", "missing"}):
                reused += 1
                documents += 1
                chunks_count += db.execute("SELECT COUNT(*) FROM chunks WHERE document_id=?",
                                           (old[0],)).fetchone()[0]
            else:
                if old:
                    doc_id = old[0]
                    db.execute("""UPDATE documents SET root=?,name=?,category=?,suffix=?,
                        modified_ms=?,size=? WHERE id=?""",
                        (item["root"], item["name"], item["category"], suffix,
                         item["modified_ms"], item["size"], doc_id))
                else:
                    cursor = db.execute("""INSERT INTO documents(
                        path,root,name,category,suffix,modified_ms,size,content_hash)
                        VALUES(?,?,?,?,?,?,?,?)""", (path, item["root"], item["name"], item["category"],
                        suffix, item["modified_ms"], item["size"], ""))
                    doc_id = cursor.lastrowid
                db.execute("SAVEPOINT extract_document")
                result = Extraction(path, ocr=args.ocr, progress=lambda count: emit(
                    task="build", phase="extract", processed=processed,
                    total=total, path=path, blocks=count,
                    documents=documents, chunks=chunks_count))
                count = indexed_chars = 0
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
                            raise FileNotFoundError("source missing; previous indexed text retained")
                        result.kind = "snapshot"
                        result.partial("preview_only:original_not_extracted")
                        blocks = [("预览快照（非完整正文）", item.get("content", ""))]
                    else:
                        blocks = result
                    for locator, piece in chunks_from_blocks(
                            blocks, item["name"], args.chunk_size, args.overlap):
                        cursor = db.execute("INSERT INTO chunks(document_id,ordinal,locator,content) VALUES(?,?,?,?)",
                            (doc_id, count, locator, piece))
                        db.execute("""INSERT INTO chunks_fts(rowid,name,path,content,locator,document_id,chunk_id)
                            VALUES(?,?,?,?,?,?,?)""", (cursor.lastrowid, fts_text(item["name"]), fts_text(path), fts_text(piece),
                            locator, doc_id, cursor.lastrowid))
                        count += 1
                        indexed_chars += len(piece)
                        digest.update(piece.encode("utf-8"))
                    if signature != source_signature(item, args):
                        raise RuntimeError("source changed during extraction; retry required")
                    if not count and result.status == "complete":
                        result.status = "no_text"
                    db.execute("UPDATE documents SET content_hash=? WHERE id=?", (digest.hexdigest(), doc_id))
                    db.execute("RELEASE extract_document")
                    updated += 1
                except Exception as error:
                    db.execute("ROLLBACK TO extract_document")
                    db.execute("RELEASE extract_document")
                    result.status = "missing" if isinstance(error, FileNotFoundError) else "failed"
                    result.issues = [str(error)[:500], "previous_chunks_retained"]
                    count, indexed_chars = db.execute(
                        "SELECT count(*),coalesce(sum(length(content)),0) FROM chunks WHERE document_id=?",
                        (doc_id,)).fetchone()
                    emit(task="build", phase="extraction-error", path=path,
                         status=result.status, error=result.issues[0])
                db.execute("""INSERT OR REPLACE INTO extraction_state(
                    document_id,source_signature,parser_version,status,issues_json,source_kind,
                    indexed_chars,extracted_blocks,attempted_at) VALUES(?,?,?,?,?,?,?,?,?)""",
                    (doc_id, signature, EXTRACTOR_VERSION, result.status,
                     json.dumps(result.issues, ensure_ascii=False), result.kind,
                     indexed_chars, result.blocks, time.strftime("%Y-%m-%dT%H:%M:%S")))
                documents += 1
                chunks_count += count
            if processed % 50 == 0:
                db.commit()
                emit(task="build", phase="build", processed=processed, total=total,
                     documents=documents, chunks=chunks_count, updated=updated, reused=reused)
        stale_ids = [row[0] for row in db.execute(
            "SELECT id,path FROM documents WHERE path NOT IN (SELECT path FROM seen_paths)"
        ) if not args.include_root or any(row[1] == root or row[1].startswith(root + os.sep)
                                          for root in args.include_root)]
        for doc_id in stale_ids:
            delete_fts_document(db, doc_id)
            db.execute("DELETE FROM documents WHERE id=?", (doc_id,))
        # Recover derived rows left by interrupted/older builders. This never
        # deletes document text, only FTS entries without a matching raw chunk.
        db.execute("""DELETE FROM chunks_fts WHERE rowid IN (
            SELECT f.rowid FROM chunks_fts f LEFT JOIN chunks c ON c.id=f.chunk_id
            WHERE c.id IS NULL OR c.document_id != f.document_id)""")
        db.execute("DELETE FROM seen_paths")
        if not args.include_root:
            db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES('index_generated_at',?)", (generated_at,))
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                   ("last_scoped_build_at" if args.include_root else "last_build_at",
                    time.strftime("%Y-%m-%dT%H:%M:%S")))
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES('extractor_version',?)", (EXTRACTOR_VERSION,))
        states = extraction_counts(db)
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES('body_source',?)",
                   ("preview" if args.snapshot_only else "mixed:original-files+legacy"
                    if states.get("legacy") else "original-files",))
        db.commit()
        final_docs = db.execute("SELECT COUNT(*) FROM documents").fetchone()[0]
        final_chunks = db.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        emit(done=True, task="build", documents=final_docs, chunks=final_chunks, updated=updated,
             reused=reused, deleted=len(stale_ids), extractionStates=states,
             scope="selected-roots" if args.include_root else "full-manifest",
             dbBytes=os.path.getsize(args.db))
    finally:
        db.close()


def query_tokens(query):
    tokens = WORD_RE.findall((query or "").casefold())
    seen = set()
    return [token for token in tokens
            if not (token in seen or seen.add(token))][:40]


def match_query(query, operator="AND"):
    tokens = query_tokens(query)
    return match_tokens(tokens, operator)


def match_tokens(tokens, operator="AND"):
    if not tokens:
        return ""
    return (" %s " % operator).join(
        '"%s"' % token.replace('"', '""') for token in tokens)


def required_relaxed_token_hits(token_count):
    if token_count <= 2:
        return token_count
    if token_count <= 6:
        return max(2, (token_count * 2 + 2) // 3)
    return max(4, (token_count * 3 + 4) // 5)


def search_rows(db, query, limit):
    limit = max(1, min(int(limit), MAX_SEARCH_LIMIT))
    tokens = query_tokens(query)
    expression = match_query(query)
    if not expression:
        return []
    weights = ",".join(str(value) for value in FTS_BM25_WEIGHTS)
    sql = """
        SELECT d.path,d.name,d.category,d.modified_ms,d.size,
               f.locator,c.content,f.name,f.path,
               bm25(chunks_fts,%s) AS rank
          FROM chunks_fts AS f
          JOIN chunks AS c ON c.id=f.chunk_id
          JOIN documents AS d ON d.id=f.document_id
         WHERE chunks_fts MATCH ?
         ORDER BY rank ASC
         LIMIT ?
    """ % weights
    fetch_limit = min(MAX_SEARCH_ROWS, limit * 6)
    rows = db.execute(sql, (expression, fetch_limit)).fetchall()
    relaxed = False
    if not rows and len(tokens) > 1:
        relaxed = True
        relaxed_tokens = [
            token for token in tokens
            if token not in RELAXED_QUERY_STOP_TOKENS
        ] or tokens
        relaxed_expression = match_tokens(relaxed_tokens, "OR")
        relaxed_limit = min(MAX_SEARCH_ROWS, max(limit * 10, 600))
        rows = db.execute(
            sql, (relaxed_expression, relaxed_limit)).fetchall()
        required = required_relaxed_token_hits(len(relaxed_tokens))
        rows = [
            row for row in rows
            if sum(token in " ".join(
                str(value or "").casefold() for value in row[6:9])
                   for token in relaxed_tokens) >= required
        ]
    best = {}
    folded = (query or "").casefold()
    for (path, name, category, modified, size, locator, content,
         _fts_name, _fts_path, rank) in rows:
        name_hit = 2 if folded and folded in name.casefold() else 0
        candidate = {
            "path": path, "name": name, "category": category,
            "modifiedMs": modified, "size": size, "locator": locator,
            "snippet": (content or "")[:420],
            "rank": float(rank), "nameHit": name_hit, "relaxed": relaxed,
        }
        old = best.get(path)
        if old is None or (name_hit, -rank) > (old["nameHit"], -old["rank"]):
            best[path] = candidate
    ordered = sorted(best.values(),
                     key=lambda row: (-row["nameHit"], row["rank"], row["name"]))
    if ordered:
        worst = max(row["rank"] for row in ordered)
        best_rank = min(row["rank"] for row in ordered)
        span = max(0.0001, worst - best_rank)
        for row in ordered:
            score_min = RELAXED_SCORE_MIN if row["relaxed"] else SCORE_MIN
            score_span = RELAXED_SCORE_SPAN if row["relaxed"] else SCORE_SPAN
            lexical = int(
                score_min + score_span * (worst - row["rank"]) / span)
            row["score"] = min(99, lexical + row["nameHit"] * 2)
            del row["rank"]
            del row["nameHit"]
    if db.execute("SELECT 1 FROM sqlite_master WHERE name='extraction_state'").fetchone():
        for row in ordered[:limit]:
            state = db.execute("""SELECT e.status,e.issues_json FROM extraction_state e
                JOIN documents d ON d.id=e.document_id WHERE d.path=?""", (row["path"],)).fetchone()
            row["extractionStatus"] = state[0] if state else "legacy"
            if state and state[0] in {"failed", "missing", "partial"}:
                row["extractionIssues"] = json.loads(state[1])
    return ordered[:limit]


def search(args):
    db = open_db(args.db, read_only=True)
    try:
        results = search_rows(db, args.query, args.limit)
        emit(done=True, task="search", query=args.query, results=results)
    finally:
        db.close()





def stats(args):
    db = open_db(args.db, read_only=True)
    try:
        documents = db.execute("SELECT COUNT(*) FROM documents").fetchone()[0]
        chunks = db.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        emit(done=True, task="stats", documents=documents, chunks=chunks,
             extractionStates=extraction_counts(db),
             dbBytes=os.path.getsize(args.db) if os.path.exists(args.db) else 0,
             lastBuild=dict(db.execute(
                 "SELECT key,value FROM meta WHERE key='last_build_at'").fetchall())
                 .get("last_build_at", ""))
    finally:
        db.close()


def issues(args):
    db = open_db(args.db, read_only=True)
    try:
        exists = db.execute("SELECT 1 FROM sqlite_master WHERE name='extraction_state'").fetchone()
        rows = db.execute("""SELECT d.path,e.status,e.issues_json,e.source_kind,e.indexed_chars
            FROM extraction_state e JOIN documents d ON d.id=e.document_id
            WHERE e.status NOT IN ('complete','no_text') ORDER BY d.path LIMIT ?""",
            (max(1, min(args.limit, 10000)),)).fetchall() if exists else []
        emit(done=True, task="issues", extractionStates=extraction_counts(db), results=[
            dict(path=p, status=s, issues=json.loads(j), extractor=k, indexedChars=n)
            for p, s, j, k, n in rows])
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
    build_parser.add_argument("--ocr", action="store_true", default=True,
                              help="OCR every image/empty PDF page (default)")
    build_parser.add_argument("--no-ocr", dest="ocr", action="store_false",
                              help="text-only pass; missing OCR is explicitly partial")
    build_parser.add_argument("--force", action="store_true", help="ignore extraction cache")
    build_parser.add_argument("--snapshot-only", action="store_true",
                              help="compatibility import; always marked partial")
    build_parser.add_argument("--include-root", action="append", default=[],
                              type=lambda value: os.path.abspath(os.path.expanduser(value)),
                              help="refresh selected manifest subtree without pruning other roots")
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
        emit(done=True, task=args.command, error=str(error)[:500])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
