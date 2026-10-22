"""Indexed failure classification and monotonic scope revisions.

Writable libraries migrate during an explicit build. Read-only older libraries
keep the bounded, streaming compatibility path; reading never migrates a file.
"""
import json
import uuid

from knowledge_library import failure_category


def prepare(db):
    columns = {row[1] for row in db.execute('PRAGMA table_info(extraction_state)')}
    if 'failure_category' not in columns:
        db.execute("ALTER TABLE extraction_state ADD COLUMN failure_category TEXT NOT NULL DEFAULT ''")
    db.execute('CREATE INDEX IF NOT EXISTS extraction_category_idx ON extraction_state(failure_category,document_id)')
    db.execute('CREATE INDEX IF NOT EXISTS extraction_failure_idx ON extraction_state(status,failure_category,document_id)')
    db.execute("INSERT OR IGNORE INTO meta VALUES('failure_epoch',?)", (uuid.uuid4().hex,))
    db.execute("INSERT OR IGNORE INTO meta VALUES('failure_revision','0')")
    # SQLite triggers also invalidate selection if another compatible writer
    # changes IDs, paths, diagnostics or ignore policy with identical counts.
    for table in ('documents', 'extraction_state', 'ignored_documents'):
        for operation in ('INSERT', 'UPDATE', 'DELETE'):
            db.execute("CREATE TRIGGER IF NOT EXISTS failure_revision_%s_%s AFTER %s ON %s BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='failure_revision'; END" % (table, operation.lower(), operation, table))
    db.execute("""CREATE TRIGGER IF NOT EXISTS failure_reclassify AFTER UPDATE OF status,issues_json ON extraction_state
        WHEN NEW.failure_category=OLD.failure_category BEGIN
        UPDATE extraction_state SET failure_category='' WHERE document_id=NEW.document_id; END""")
    # Cursor iteration bounds memory, even for a large first-time migration.
    for ident, status, raw in db.execute("SELECT document_id,status,issues_json FROM extraction_state WHERE failure_category='' OR failure_category IS NULL"):
        try: issues=json.loads(raw)
        except (ValueError,TypeError): issues=['无法读取旧失败记录']
        db.execute('UPDATE extraction_state SET failure_category=? WHERE document_id=?', (failure_category(status,issues), ident))


def store_category(db, ident, status, issues):
    db.execute('UPDATE extraction_state SET failure_category=? WHERE document_id=?',
               (failure_category(status, issues), ident))
