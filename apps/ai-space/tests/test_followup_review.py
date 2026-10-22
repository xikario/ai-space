"""Synthetic regressions for review follow-up; never read a personal library."""
import argparse
import contextlib
import fcntl
import hashlib
import io
import json
import os
import signal
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import failure_store
import knowledge_library as lib
import knowledge_maintenance as maintenance
import ocr_budget
import ocr_cache
import smart_space_knowledge as worker
import snapshot_io
from text_safety import text_encoding


class FollowupReview(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="followup-review-")
        self.root = Path(self.temp.name)
        self.path = self.root / "library.sqlite"
        self.patch = mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")})
        self.patch.start()

    def tearDown(self):
        self.patch.stop()
        self.temp.cleanup()

    def database(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return worker.open_db(str(self.path))

    def add(self, db, ident, status="partial", issues=None, kind="pdf", original=None):
        original = original or self.root / ("missing-%s.pdf" % ident)
        db.execute(
            "INSERT INTO documents(id,path,name,content_hash) VALUES(?,?,?,?)",
            (ident, str(original), original.name, "hash"),
        )
        db.execute(
            "INSERT INTO chunks(id,document_id,ordinal,content) VALUES(?,?,0,'retained body')",
            (ident, ident),
        )
        db.execute(
            "INSERT INTO chunks_fts(rowid,document_id,chunk_id,content) VALUES(?,?,?,'retained body')",
            (ident, ident, ident),
        )
        db.execute(
            "INSERT INTO document_names_fts(rowid,name,document_id) VALUES(?,?,?)",
            (ident, original.name, ident),
        )
        db.execute(
            "INSERT INTO extraction_state(document_id,status,issues_json,source_kind) VALUES(?,?,?,?)",
            (ident, status, json.dumps(issues or []), kind),
        )
        failure_store.store_metrics(db, ident, status, issues or [], kind)
        worker.mark_availability(db, ident, False)
        return original

    def old_absent(self, db, ident):
        db.execute(
            "UPDATE document_availability SET absent_since='2000-01-01T00:00:00Z' WHERE document_id=?",
            (ident,),
        )

    def media_fixture(self, suffix=".png", signature="unrecognized"):
        path = self.root / ("scanned" + suffix)
        path.write_bytes(b"unchanged source")
        args = worker.parser().parse_args(["build", "--db", str(self.path)])
        info = path.stat()
        if signature == "legacy":
            signature = hashlib.sha256(
                json.dumps(
                    [
                        worker.parser_version(path),
                        (info.st_mtime_ns, info.st_size),
                        1200,
                        120,
                        True,
                        False,
                    ],
                    sort_keys=True,
                ).encode()
            ).hexdigest()
        with self.database() as db:
            self.add(
                db, 1, status="complete", kind="image" if suffix == ".png" else "pdf", original=path
            )
            db.execute(
                "UPDATE extraction_state SET source_signature=?,parser_version=?,indexed_chars=13",
                (signature, worker.parser_version(path)),
            )
        item = dict(
            path=str(path),
            name=path.name,
            root=str(self.root),
            category="document",
            is_dir=False,
            size=info.st_size,
            modified_ms=info.st_mtime_ns // 1000000,
        )
        return path, args, item

    def media_build(self, args, item, extraction=None):
        output = io.StringIO()
        with mock.patch.object(
            worker, "index_items", return_value=iter([(1, item)])
        ), mock.patch.object(
            worker,
            "Extraction",
            extraction or mock.Mock(side_effect=AssertionError("unexpected extraction")),
        ), contextlib.redirect_stdout(
            output
        ):
            worker.build_locked(args)
        return json.loads(output.getvalue().splitlines()[-1])

    def test_legacy_media_ocr_is_reused_and_signature_upgraded(self):
        path, args, item = self.media_fixture(signature="legacy")
        result = self.media_build(args, item)
        self.assertEqual(
            (result["reused"], result["updated"], result["legacySignaturesReused"]), (1, 0, 1)
        )
        with lib.open_readonly(self.path) as db:
            self.assertEqual(
                db.execute("SELECT content FROM chunks").fetchone()[0], "retained body"
            )
            self.assertEqual(
                db.execute("SELECT source_signature FROM extraction_state").fetchone()[0],
                worker.source_signature(item, args, ocr=True),
            )
        self.assertEqual(self.media_build(args, item)["legacySignaturesReused"], 0)

    def test_legacy_media_compatibility_rejects_changed_source_or_policy(self):
        path, args, item = self.media_fixture(signature="legacy")
        with lib.open_readonly(self.path) as db:
            state = db.execute(
                "SELECT source_signature,status,parser_version,indexed_chars FROM extraction_state"
            ).fetchone()

        def source():
            info = path.stat()
            return info.st_dev, info.st_ino, info.st_mtime_ns, info.st_ctime_ns, info.st_size

        self.assertIsNotNone(worker.compatible_media_signature(item, args, state, source()))
        for key, value in (
            ("force", True),
            ("ocr", True),
            ("snapshot_only", True),
            ("chunk_size", 600),
        ):
            with mock.patch.object(args, key, value):
                self.assertIsNone(worker.compatible_media_signature(item, args, state, source()))
        self.assertIsNone(
            worker.compatible_media_signature(
                item, args, (state[0], state[1], "different-parser", state[3]), source()
            )
        )
        path.write_bytes(b"new source with different length")
        self.assertIsNone(worker.compatible_media_signature(item, args, state, source()))

    def test_text_only_refresh_retains_old_image_and_partial_pdf_ocr(self):
        for suffix, issue in ((".png", "image_needs_ocr"), (".pdf", "page_needs_ocr:2")):
            self.path = self.root / ("media" + suffix + ".sqlite")
            path, args, item = self.media_fixture(suffix)

            class Partial:
                status = "partial"
                kind = "image" if suffix == ".png" else "pdf"
                blocks = 1

                def __init__(self, *unused, **kwargs):
                    self.issues = [issue]

                def __iter__(self):
                    return iter([] if suffix == ".png" else [("第 1 页", "a new text layer")])

                def partial(self, reason):
                    self.issues.append(reason)

            result = self.media_build(args, item, Partial)
            self.assertEqual((result["retainedOcr"], result["updated"]), (1, 0))
            with lib.open_readonly(self.path) as db:
                self.assertEqual(
                    db.execute("SELECT content FROM chunks").fetchone()[0], "retained body"
                )
                state, issues = db.execute(
                    "SELECT status,issues_json FROM extraction_state"
                ).fetchone()
                self.assertEqual(state, "partial")
                self.assertIn("previous_ocr_body_retained", issues)
                self.assertEqual(db.execute("PRAGMA quick_check").fetchone()[0], "ok")
            self.assertEqual(self.media_build(args, item)["reused"], 1)

            class Full:
                status = "complete"
                kind = "image" if suffix == ".png" else "pdf"
                blocks = 1
                issues = []

                def __init__(self, *unused, **kwargs):
                    pass

                def __iter__(self):
                    return iter([("新 OCR", "newly recognized original content")])

            args.ocr = True
            args.ocr_only = True
            result = self.media_build(args, item, Full)
            self.assertEqual((result["retainedOcr"], result["updated"]), (0, 1))
            with lib.open_readonly(self.path) as db:
                self.assertIn(
                    "newly recognized", db.execute("SELECT content FROM chunks").fetchone()[0]
                )
                self.assertEqual(
                    db.execute("SELECT status FROM extraction_state").fetchone()[0], "complete"
                )

    def test_supervisor_pause_while_waiting_for_quiet_child(self):
        wrapper = """import sys,json
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import knowledge_jobs as jobs
original = jobs.subprocess.Popen
def child(command, **kwargs):
    if any(str(part).endswith('/smart_space_indexer.py') for part in command):
        return original([sys.executable, "-c", "import json,time;print(json.dumps(dict(phase='quiet-child')),flush=True);time.sleep(60)"], **kwargs)
    return original(command, **kwargs)
jobs.subprocess.Popen = child
sys.argv = ['knowledge_jobs', '--directory', sys.argv[2], '--roots-json', json.dumps([str(Path(sys.argv[2]).parent)])]
raise SystemExit(jobs.main())
"""
        process = subprocess.Popen(
            [sys.executable, "-c", wrapper, str(SCRIPTS), str(self.root / "job")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            while True:
                line = process.stdout.readline()
                self.assertTrue(line, "supervisor exited before entering select")
                if json.loads(line).get("phase") == "quiet-child":
                    break
            process.send_signal(signal.SIGTERM)
            output, errors = process.communicate(timeout=6)
            final = json.loads(output.splitlines()[-1])
            self.assertTrue(final["paused"], errors)
            self.assertIn("已提交进度保留", final["error"])
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            process.stdout.close()
            process.stderr.close()

    def test_physical_floor_independent_of_virtual_budget(self):
        with mock.patch.object(
            ocr_budget, "available_memory", return_value=512 * 1024 ** 2
        ), mock.patch.dict(os.environ, AI_SPACE_OCR_MEMORY_MB="8192"):
            ocr_budget.check_memory_pressure("建库启动")
            with tempfile.TemporaryFile() as output:
                result = ocr_budget.run_ocr(
                    [sys.executable, "-c", 'print("OK")'], stdout=output, timeout=3
                )
                self.assertEqual(result.returncode, 0)
        with mock.patch.object(ocr_budget, "available_memory", return_value=383 * 1024 ** 2):
            with self.assertRaisesRegex(ocr_budget.ResourcePressureError, "383.*384"):
                ocr_budget.check_memory_pressure("建库启动")
        with mock.patch.object(ocr_budget, "available_memory", return_value=255 * 1024 ** 2):
            with self.assertRaises(ocr_budget.ResourcePressureError):
                ocr_budget.check_memory_pressure("运行", starting=False)

    @unittest.skipUnless(hasattr(os, "sched_getaffinity"), "Linux affinity")
    def test_ocr_keeps_allowed_cpu_set(self):
        with mock.patch.object(ocr_budget, "available_memory", return_value=None), mock.patch.dict(
            os.environ, AI_SPACE_OCR_CPU_PERCENT="100"
        ):
            with tempfile.TemporaryFile() as output:
                result = ocr_budget.run_ocr(
                    [
                        sys.executable,
                        "-c",
                        'import os;print(",".join(map(str,sorted(os.sched_getaffinity(0)))))',
                    ],
                    stdout=output,
                    timeout=3,
                )
                output.seek(0)
                reported = output.read()
        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            set(map(int, reported.decode().strip().split(","))), os.sched_getaffinity(0)
        )

    def test_summary_avoids_decoding_thousands_of_partial_diagnostics(self):
        db = self.database()
        for ident in range(1, 3001):
            self.add(
                db,
                ident,
                issues=[
                    "format_limited",
                    "page_needs_ocr:2",
                    "page_needs_ocr:2",
                    "empty_ocr_page:3",
                ],
            )
        db.execute("INSERT INTO ignored_documents VALUES(1,'now')")
        db.commit()
        db.close()
        with mock.patch.object(
            lib.json, "loads", side_effect=AssertionError("large diagnostic scan")
        ):
            answer = lib.stats(self.path, True, True)
        self.assertEqual(answer["ocrPending"]["documents"], 2999)
        self.assertEqual(answer["ocrPending"]["pdfPages"], 2999)
        self.assertEqual(answer["ocrPending"]["emptyOcrPages"], 3000)
        self.assertEqual(answer["ocrPending"]["statisticsSource"], "summary")

    def test_legacy_fast_stats_unknown_then_explicit_upgrade(self):
        db = self.database()
        self.add(db, 1, issues=["page_needs_ocr:1"])
        db.execute("DELETE FROM extraction_summary")
        db.commit()
        db.close()
        before = self.path.read_bytes()
        with mock.patch.object(lib.json, "loads", side_effect=AssertionError("legacy scan")):
            answer = lib.stats(self.path, True, True)
        self.assertFalse(answer["ocrPending"]["complete"])
        self.assertIsNone(answer["ocrPending"]["documents"])
        self.assertEqual(before, self.path.read_bytes())
        db = self.database()
        db.close()
        self.assertEqual(lib.stats(self.path, True, True)["ocrPending"]["pdfPages"], 1)

    def test_changed_issues_invalidate_summary_until_build(self):
        db = self.database()
        self.add(db, 1, issues=["page_needs_ocr:1"])
        db.commit()
        db.execute(
            'UPDATE extraction_state SET issues_json=\'["page_needs_ocr:5","page_needs_ocr:6"]\''
        )
        db.commit()
        db.close()
        self.assertFalse(lib.stats(self.path, True, True)["ocrPending"]["complete"])
        db = self.database()
        db.close()
        self.assertEqual(lib.stats(self.path, True, True)["ocrPending"]["pdfPages"], 2)

    def test_upgrade_full_has_specific_diagnostic_and_keeps_body(self):
        db = self.database()
        self.add(db, 1)
        db.execute("DELETE FROM meta WHERE key='fts_rowid_layout'")
        db.commit()
        db.close()
        output = io.StringIO()
        with mock.patch.object(
            worker,
            "align_fts_rowids",
            side_effect=sqlite3.OperationalError("database or disk is full"),
        ), contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(RuntimeError, "旧库索引升级.*额外.*配额"):
                worker.open_db(str(self.path))
        self.assertIn('"phase":"migrate"', output.getvalue())
        with lib.open_readonly(self.path) as check:
            self.assertEqual(
                check.execute("SELECT content FROM chunks").fetchone()[0], "retained body"
            )
            self.assertIn("正文索引布局", worker.upgrade_plan(check))

    def test_absent_age_preserved_and_present_resets(self):
        db = self.database()
        self.add(db, 1)
        self.old_absent(db, 1)
        worker.mark_availability(db, 1, False)
        self.assertEqual(
            db.execute("SELECT absent_since FROM document_availability").fetchone()[0],
            "2000-01-01T00:00:00Z",
        )
        worker.mark_availability(db, 1, True)
        self.assertEqual(
            db.execute("SELECT absent_since FROM document_availability").fetchone()[0], ""
        )
        worker.mark_availability(db, 1, False)
        self.assertNotEqual(
            db.execute("SELECT absent_since FROM document_availability").fetchone()[0],
            "2000-01-01T00:00:00Z",
        )
        db.close()

    def test_cleanup_preview_confirm_skips_reappeared_and_keeps_recent(self):
        db = self.database()
        self.add(db, 1)
        present = self.root / "returned.pdf"
        present.write_text("original")
        self.add(db, 2, original=present)
        self.add(db, 3)
        self.old_absent(db, 1)
        self.old_absent(db, 2)
        db.commit()
        db.close()
        preview = maintenance.prune_absent(self.path, 30)
        self.assertEqual(preview["count"], 2)
        self.assertEqual(lib.stats(self.path)["documents"], 3)
        with contextlib.redirect_stdout(io.StringIO()):
            result = maintenance.prune_absent(
                self.path, 30, preview["cutoff"], preview["selection_token"], True, 0.01
            )
        self.assertEqual((result["deleted"], result["restored"]), (1, 1))
        self.assertFalse(result["files_deleted"])
        self.assertEqual(present.read_text(), "original")
        with lib.open_readonly(self.path) as db:
            self.assertEqual(
                db.execute("SELECT id FROM documents ORDER BY id").fetchall(), [(2,), (3,)]
            )
            self.assertFalse(db.execute("SELECT 1 FROM chunks_fts WHERE rowid=1").fetchone())
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_cleanup_rejects_changed_scope_and_busy_pipeline(self):
        db = self.database()
        self.add(db, 1)
        self.old_absent(db, 1)
        db.commit()
        preview = maintenance.prune_absent(self.path, 30)
        db.execute("UPDATE documents SET name='changed'")
        db.commit()
        db.close()
        with self.assertRaisesRegex(ValueError, "已变化"):
            maintenance.prune_absent(
                self.path, 30, preview["cutoff"], preview["selection_token"], True, 0.01
            )
        with (self.root / ".pipeline.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(RuntimeError, "建库正在运行"):
                maintenance.prune_absent(
                    self.path, 30, preview["cutoff"], preview["selection_token"], True, 0.01
                )
        self.assertEqual(lib.stats(self.path)["documents"], 1)

    def test_cleanup_cancellation_rolls_back_deletions(self):
        db = self.database()
        for ident in (1, 2):
            self.add(db, ident)
            self.old_absent(db, ident)
        db.commit()
        db.close()
        preview = maintenance.prune_absent(self.path, 30)
        original = worker.delete_fts_document
        calls = []

        def interrupted(db, ident):
            calls.append(ident)
            if len(calls) == 2:
                raise InterruptedError("cancel")
            original(db, ident)

        with mock.patch.object(
            worker, "delete_fts_document", side_effect=interrupted
        ), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(InterruptedError):
                maintenance.prune_absent(
                    self.path, 30, preview["cutoff"], preview["selection_token"], True, 0.01
                )
        self.assertEqual(lib.stats(self.path)["documents"], 2)
        with lib.open_readonly(self.path) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM chunks_fts").fetchone()[0], 2)

    def test_page_cache_removes_only_unlocked_temporary_files(self):
        folder = self.root / "cache"
        folder.mkdir()
        orphan = folder / ".page-abcdefgh"
        orphan.write_text("orphan")
        active = folder / ".page-ijklmnop"
        active.write_text("active")
        target = self.root / "outside"
        target.write_text("keep")
        (folder / ".page-qrstuvwx").symlink_to(target)
        with active.open("rb") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            cache = ocr_cache.PageCache(folder)
            self.assertFalse(orphan.exists())
            self.assertTrue(active.exists())
            self.assertEqual(target.read_text(), "keep")
            cache.db.close()
        cache = ocr_cache.PageCache(folder)
        cache.db.close()
        self.assertFalse(active.exists())

    def test_default_scratch_does_not_scan_system_tmp(self):
        with mock.patch.object(Path, "iterdir", side_effect=AssertionError("global /tmp scan")):
            self.assertEqual(snapshot_io.disposable_owner(None), Path(tempfile.gettempdir()))

    def test_bomless_utf16_and_binary_still_distinct(self):
        text = "English log with Chinese 中文 and normal lines\n" * 10
        for encoding in ("utf-16-le", "utf-16-be"):
            self.assertEqual(text_encoding(text.encode(encoding)), encoding)
        for binary in (b"\x00" * 100, b"\x7fELF\x00\x01\x02" * 100, (b"G" + b"\xff" * 187) * 4):
            with self.assertRaises(ValueError):
                text_encoding(binary)

    def test_time_checkpoint_retains_completed_short_batch(self):
        paths = []
        for name in ("one.txt", "two.txt"):
            path = self.root / name
            path.write_text("正文资料")
            paths.append(path)
        clock = [0]

        def items(*unused):
            for path in paths + [self.root]:
                clock[0] += 6
                yield 3, dict(
                    path=str(path),
                    name=path.name,
                    root=str(self.root),
                    category="document",
                    is_dir=path.is_dir(),
                    modified_ms=0,
                    size=0,
                    content="",
                )
            raise InterruptedError("simulated pause between documents")

        args = argparse.Namespace(
            db=str(self.path),
            index_stream=None,
            index_json=None,
            include_root=[],
            force=False,
            ocr=False,
            ocr_only=False,
            snapshot_only=False,
            chunk_size=1200,
            overlap=120,
        )
        with mock.patch.object(worker, "index_items", side_effect=items), mock.patch.object(
            worker.time, "monotonic", side_effect=lambda: clock[0]
        ), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(InterruptedError):
                worker.build_locked(args)
        self.assertEqual(lib.stats(self.path)["documents"], 2)

    def test_real_sqlite_quota_failure_preserves_text_and_explains_upgrade(self):
        db = self.database()
        self.add(db, 1)
        body = " ".join(hashlib.sha256(str(i).encode()).hexdigest() for i in range(5000))
        db.execute("UPDATE chunks SET content=?", (body,))
        db.execute("UPDATE chunks_fts SET rowid=1001,content=?", (worker.fts_text(body),))
        db.execute("DELETE FROM meta WHERE key='fts_rowid_layout'")
        db.commit()
        db.execute("VACUUM")
        db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        pages = db.execute("PRAGMA page_count").fetchone()[0]
        page_size = db.execute("PRAGMA page_size").fetchone()[0]
        db.close()
        with mock.patch.dict(
            os.environ, AI_SPACE_MAX_GIB=str(pages * page_size / 1024 ** 3)
        ), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "旧库索引升级.*容量上限"):
                worker.open_db(str(self.path))
        with contextlib.closing(lib.open_readonly(self.path)) as db:
            self.assertEqual(db.execute("SELECT content FROM chunks").fetchone()[0], body)
            self.assertEqual(db.execute("PRAGMA quick_check").fetchone()[0], "ok")

    def test_stage_protocol_keeps_actionable_error_and_bounds_lines(self):
        import knowledge_jobs

        message = "旧库索引升级需要额外空间，请检查单库上限"
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import json;print(json.dumps(dict(done=True,error=%r)));raise SystemExit(1)"
                % message,
            ],
            stdout=subprocess.PIPE,
        )
        with contextlib.redirect_stdout(io.StringIO()):
            result = knowledge_jobs.forward_stage_output(process, lambda: None)
        self.assertEqual(result["error"], message)
        self.assertEqual(process.wait(), 1)
        process = subprocess.Popen(
            [sys.executable, "-c", "import sys;sys.stdout.write('x'*1200000)"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        try:
            with self.assertRaisesRegex(ValueError, "单条响应过大"):
                knowledge_jobs.forward_stage_output(process, lambda: None)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()

    def test_export_uses_delete_journal_on_readonly_media(self):
        db = self.database()
        self.add(db, 1)
        db.commit()
        db.close()
        folder = self.root / "readonly-media"
        folder.mkdir()
        package = folder / "shared.aikb"
        with contextlib.redirect_stdout(io.StringIO()):
            lib.export_library(self.path, package, "example", 0.01, 1)
        folder.chmod(0o555)
        try:
            with contextlib.closing(lib.open_readonly(package)) as db:
                self.assertEqual(db.execute("PRAGMA journal_mode").fetchone()[0], "delete")
                self.assertEqual(db.execute("SELECT count(*) FROM chunks").fetchone()[0], 1)
            self.assertFalse(Path(str(package) + "-shm").exists())
            self.assertFalse(Path(str(package) + "-wal").exists())
        finally:
            folder.chmod(0o755)

    def test_cross_library_budget_is_shared(self):
        clock = [0]

        def search(*args):
            self.assertLessEqual(args[3], 1000)
            clock[0] = 2.1
            return dict(results=[], timed_out=False, truncated=False, body_searched=True)

        with mock.patch.object(
            lib, "selected_paths", return_value=[dict(path="first"), dict(path="second")]
        ), mock.patch.object(lib, "search_one", side_effect=search) as query, mock.patch.object(
            lib.time, "monotonic", side_effect=lambda: clock[0]
        ):
            answer = lib.search_libraries("example", all_libraries=True, budget_ms=2000)
        self.assertEqual(query.call_count, 1)
        self.assertEqual(answer["libraries"][1]["status"], "not_queried")
        self.assertTrue(answer["incomplete"])

    def test_cleanup_rejects_registered_readonly_legacy(self):
        db = self.database()
        self.add(db, 1)
        self.old_absent(db, 1)
        db.commit()
        db.close()
        with lib.registry_write() as catalog:
            catalog["libraries"].append(
                dict(
                    path=str(self.path),
                    read_only=True,
                    library_id="00ba3702-1fcd-4b94-a281-fb49af624027",
                    version="1",
                    bytes=self.path.stat().st_size,
                    mtime_ns=self.path.stat().st_mtime_ns,
                    format="legacy-sqlite",
                )
            )
        with self.assertRaisesRegex(ValueError, "只读"):
            maintenance.prune_absent(self.path)
        self.assertEqual(lib.stats(self.path)["documents"], 1)

    def test_cleanup_honors_storage_quota_and_rolls_back(self):
        db = self.database()
        self.add(db, 1)
        self.old_absent(db, 1)
        db.commit()
        db.close()
        preview = maintenance.prune_absent(self.path, 30)
        with self.assertRaisesRegex(RuntimeError, "单库容量.*回滚"):
            maintenance.prune_absent(
                self.path, 30, preview["cutoff"], preview["selection_token"], True, 0.01, 0.000001
            )
        self.assertEqual(lib.stats(self.path)["documents"], 1)

    def test_cleanup_late_quota_failure_rolls_back_body_and_indexes(self):
        db = self.database()
        self.add(db, 1)
        self.old_absent(db, 1)
        db.commit()
        db.close()
        preview = maintenance.prune_absent(self.path, 30)
        with mock.patch.object(
            maintenance, "guard_storage", side_effect=[None, lib.StorageLimitError("full")]
        ), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "回滚"):
                maintenance.prune_absent(
                    self.path, 30, preview["cutoff"], preview["selection_token"], True, 0.01
                )
        self.assertEqual(lib.stats(self.path)["documents"], 1)
        with contextlib.closing(lib.open_readonly(self.path)) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM chunks_fts").fetchone()[0], 1)


if __name__ == "__main__":
    unittest.main()
