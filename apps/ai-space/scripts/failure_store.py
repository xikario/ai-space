"""Indexed failure classification and monotonic scope revisions.

Writable libraries migrate during an explicit build. Read-only older libraries
keep the bounded, streaming compatibility path; reading never migrates a file.
"""
import json
import uuid

from knowledge_library import failure_category


def prepare(db):
    columns = {row[1] for row in db.execute("PRAGMA table_info(extraction_state)")}
    if "failure_category" not in columns:
        db.execute(
            "ALTER TABLE extraction_state ADD COLUMN failure_category TEXT NOT NULL DEFAULT ''"
        )
    db.execute(
        "CREATE INDEX IF NOT EXISTS extraction_category_idx ON extraction_state(failure_category,document_id)"
    )
    db.execute(
        "CREATE INDEX IF NOT EXISTS extraction_failure_idx ON extraction_state(status,failure_category,document_id)"
    )
    db.execute("INSERT OR IGNORE INTO meta VALUES('failure_epoch',?)", (uuid.uuid4().hex,))
    db.execute("INSERT OR IGNORE INTO meta VALUES('failure_revision','0')")
    # SQLite triggers also invalidate selection if another compatible writer
    # changes IDs, paths, diagnostics or ignore policy with identical counts.
    for table in ("documents", "extraction_state", "ignored_documents", "document_availability"):
        for operation in ("INSERT", "UPDATE", "DELETE"):
            db.execute(
                "CREATE TRIGGER IF NOT EXISTS failure_revision_%s_%s AFTER %s ON %s BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='failure_revision'; END"
                % (table, operation.lower(), operation, table)
            )
    db.execute(
        """CREATE TRIGGER IF NOT EXISTS failure_reclassify AFTER UPDATE OF status,issues_json ON extraction_state
        WHEN NEW.failure_category=OLD.failure_category BEGIN
        UPDATE extraction_state SET failure_category='' WHERE document_id=NEW.document_id; END"""
    )
    # Cursor iteration bounds memory, even for a large first-time migration.
    for ident, status, raw in db.execute(
        "SELECT document_id,status,issues_json FROM extraction_state WHERE failure_category='' OR failure_category IS NULL"
    ):
        try:
            issues = json.loads(raw)
        except (ValueError, TypeError):
            issues = ["无法读取旧失败记录"]
        db.execute(
            "UPDATE extraction_state SET failure_category=? WHERE document_id=?",
            (failure_category(status, issues), ident),
        )


def store_category(db, ident, status, issues):
    db.execute(
        "UPDATE extraction_state SET failure_category=? WHERE document_id=?",
        (failure_category(status, issues), ident),
    )


def extraction_metrics(status, issues, kind):
    values = {str(value) for value in issues} if isinstance(issues, list) else set()
    return (
        len({v for v in values if v.startswith("page_needs_ocr:")}),
        int("image_needs_ocr" in values),
        len({v for v in values if v.startswith("empty_ocr_page:")}),
        int(kind == "image" and status == "no_text"),
    )


def store_metrics(db, ident, status, issues, kind):
    db.execute(
        "INSERT OR REPLACE INTO extraction_summary VALUES(?,?,?,?,?,?)",
        (ident, status, *extraction_metrics(status, issues, kind)),
    )


def prepare_metrics(db):
    db.execute(
        """CREATE TABLE IF NOT EXISTS extraction_summary(
        document_id INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,
        status TEXT NOT NULL,pending_pages INTEGER NOT NULL,pending_images INTEGER NOT NULL,
        empty_pages INTEGER NOT NULL,empty_images INTEGER NOT NULL)"""
    )
    db.execute("CREATE INDEX IF NOT EXISTS extraction_summary_status ON extraction_summary(status)")
    db.execute(
        "CREATE INDEX IF NOT EXISTS extraction_status_idx ON extraction_state(status,document_id)"
    )
    # Invalidate derived counters when an older/independent writer changes a
    # record. A reader never upgrades a database or reports unknown counts as 0.
    for operation in ("INSERT", "UPDATE OF status,issues_json,source_kind", "DELETE"):
        label = operation.split()[0].lower()
        row = "OLD" if label == "delete" else "NEW"
        db.execute(
            "CREATE TRIGGER IF NOT EXISTS extraction_summary_%s AFTER %s ON extraction_state BEGIN DELETE FROM extraction_summary WHERE document_id=%s.document_id; END"
            % (label, operation, row)
        )
    db.execute(
        "INSERT OR IGNORE INTO extraction_summary SELECT document_id,status,0,0,0,0 FROM extraction_state WHERE status='complete'"
    )
    for ident, status, raw, kind in db.execute(
        "SELECT e.document_id,e.status,e.issues_json,e.source_kind FROM extraction_state e LEFT JOIN extraction_summary s ON s.document_id=e.document_id WHERE s.document_id IS NULL"
    ):
        try:
            issues = json.loads(raw)
        except (ValueError, TypeError):
            issues = []
        store_metrics(db, ident, status, issues, kind)
