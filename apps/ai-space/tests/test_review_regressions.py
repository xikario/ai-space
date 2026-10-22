import contextlib
import errno
import fcntl
import gzip
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import tracemalloc
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import failure_store
import gvariant_json
import knowledge_library as lib
import ocr_cache
import ocr_environment
import smart_space_extract as extract
import smart_space_indexer as indexer
import smart_space_knowledge as worker
from snapshot_io import ReuseMap, RecordList, records


class ReviewRegressions(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.environment = mock.patch.dict(
            os.environ,
            {
                "XDG_CONFIG_HOME": str(self.root / "config"),
                "AI_SPACE_KNOWLEDGE_DB": str(self.root / "absent.sqlite"),
            },
        )
        self.environment.start()

    def tearDown(self):
        self.environment.stop()
        self.temp.cleanup()

    def silence(self):
        return contextlib.redirect_stdout(io.StringIO())

    def test_registry_recovers_good_rows_and_backs_up_original_bytes(self):
        path = lib.registry_path()
        path.parent.mkdir(parents=True)
        row = dict(
            path=str(self.root / "share.aikb"),
            library_id="00ba3702-1fcd-4b94-a281-fb49af624027",
            version="1",
            bytes=1,
            mtime_ns=1,
            read_only=True,
        )
        raw = json.dumps(
            dict(version=1, libraries=[row, {"path": "invalid"}]), ensure_ascii=False
        ).encode()
        path.write_bytes(raw)
        self.assertEqual(lib.registry()["libraries"], [row])
        self.assertTrue(lib.registry()["registry_warnings"])
        result = lib.repair_registry()
        self.assertEqual(Path(result["backup"]).read_bytes(), raw)
        self.assertEqual(lib.registry()["libraries"], [row])
        self.assertFalse(lib.registry()["registry_warnings"])
        self.assertFalse(result["files_deleted"])

    def test_unreadable_mounted_path_does_not_hide_other_libraries(self):
        good = self.root / "good.aikb"
        good.write_bytes(b"example")
        info = good.stat()
        with lib.registry_write() as catalog:
            for identifier, path in (
                ("00ba3702-1fcd-4b94-a281-fb49af624027", str(good)),
                ("11ba3702-1fcd-4b94-a281-fb49af624027", "/" + "x" * 5000),
            ):
                catalog["libraries"].append(
                    dict(
                        path=path,
                        library_id=identifier,
                        version="1",
                        bytes=info.st_size,
                        mtime_ns=info.st_mtime_ns,
                        read_only=True,
                    )
                )
        selected = lib.selected_paths(str(self.root / "absent"), True, True)
        self.assertEqual(len(selected), 2)
        self.assertTrue(selected[0]["available"])
        self.assertFalse(selected[1]["available"])
        self.assertIn("无法读取", selected[1]["availability_error"])

    def test_broken_registry_has_explicit_recovery(self):
        path = lib.registry_path()
        path.parent.mkdir(parents=True)
        path.write_bytes(b"{broken")
        self.assertTrue(lib.registry()["repair_required"])
        result = lib.repair_registry()
        self.assertEqual(Path(result["backup"]).read_bytes(), b"{broken")
        self.assertEqual(lib.registry()["libraries"], [])

    def test_utf8_registry_under_ascii_locale(self):
        with lib.registry_write() as catalog:
            catalog["description"] = "中文知识库"
        self.assertIn("中文知识库", lib.registry_path().read_text(encoding="utf-8"))

    def test_no_replace_fallback_without_rename_or_links(self):
        source = self.root / "staged"
        target = self.root / "shared.aikb"
        source.write_bytes(b"knowledge" * 10000)
        with mock.patch.object(lib.ctypes, "CDLL", return_value=object()), mock.patch.object(
            lib.os, "link", side_effect=OSError(errno.ENOTSUP, "no links")
        ):
            lib.publish_no_replace(source, target)
        self.assertEqual(target.read_bytes(), b"knowledge" * 10000)
        self.assertFalse(source.exists())
        other = self.root / "other"
        other.write_bytes(b"new")
        with mock.patch.object(lib.ctypes, "CDLL", return_value=object()), mock.patch.object(
            lib.os, "link", side_effect=OSError(errno.ENOTSUP, "no links")
        ):
            with self.assertRaises(FileExistsError):
                lib.publish_no_replace(other, target)
        self.assertEqual(target.read_bytes(), b"knowledge" * 10000)

    def test_non_posix_permissions_do_not_prevent_sealed_export(self):
        path = self.root / "library.sqlite"
        db = worker.open_db(str(path))
        db.close()
        target = self.root / "shared.aikb"
        with mock.patch.object(
            lib.os, "chmod", side_effect=OSError(errno.EOPNOTSUPP, "no POSIX modes")
        ), self.silence():
            lib.export_library(path, target, "test", 0.01, 1)
        self.assertEqual(lib.inspect_package(target)["format"], "aikb")
        with self.assertRaisesRegex(ValueError, "read-only"):
            worker.open_db(str(target))

    def test_fallback_does_not_overwrite_dangling_link(self):
        source = self.root / "staged"
        source.write_bytes(b"knowledge")
        target = self.root / "shared.aikb"
        target.symlink_to(self.root / "absent")
        with mock.patch.object(lib.ctypes, "CDLL", return_value=object()), mock.patch.object(
            lib.os, "link", side_effect=OSError(errno.ENOTSUP, "no links")
        ):
            with self.assertRaises(FileExistsError):
                lib.publish_no_replace(source, target)
        self.assertTrue(target.is_symlink())
        self.assertTrue(source.exists())

    def test_gvariant_contract_accepts_string_and_variant_and_rejects_wrong_type(self):
        payload = json.dumps({"items": [{"name": "中文"}]}, ensure_ascii=False)
        for wire in (
            "(" + repr(payload) + ",)",
            "(<" + repr(payload) + ">,)",
            "(string " + repr(payload) + ",)",
            "(@s " + repr(payload) + ",)",
        ):
            self.assertEqual(gvariant_json.parse(wire.encode())["items"][0]["name"], "中文")
        for wire in (b"(uint32 5,)", b"(@as [],)", b"('unfinished)"):
            with self.assertRaises(ValueError):
                gvariant_json.parse(wire)

    def test_pdf_encryption_and_missing_languages_are_actionable_categories(self):
        self.assertEqual(
            lib.failure_category("failed", ["pdftotext failed (1): Incorrect password"]),
            "encrypted",
        )
        self.assertEqual(lib.failure_category("failed", ["ocr_languages_missing:chi_sim"]), "tools")
        self.assertIn("tesseract-ocr-chi-sim", lib.describe_issue("ocr_languages_missing:chi_sim"))
        self.assertIn("已加密", lib.describe_issue("pdftotext failed (1): Incorrect password"))

    def test_language_preflight_reports_exact_missing_pack(self):
        ocr_environment._preflight.cache_clear()
        result = subprocess.CompletedProcess(
            [], 0, stdout=b"List of available languages (1):\neng\n", stderr=b""
        )
        with mock.patch.object(ocr_environment.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "ocr_languages_missing:chi_sim"):
                ocr_environment._preflight("/fake/tesseract", (1, 2, 3), "chi_sim+eng")
        ocr_environment._preflight.cache_clear()

    def test_ocr_cache_uses_ledger_not_directory_scan_per_page(self):
        folder = self.root / "cache"
        cache = ocr_cache.PageCache(folder)
        try:
            with mock.patch.object(
                ocr_cache.os, "scandir", side_effect=AssertionError("per-page scan")
            ):
                for index in range(100):
                    cache.save(folder / ("%064x.txt" % index), "page" * 25, 500)
            self.assertLessEqual(cache.db.execute("SELECT total FROM counters").fetchone()[0], 500)
            self.assertEqual(len(list(folder.glob("*.txt"))), 5)
        finally:
            cache.db.close()

    def test_empty_ocr_pages_are_also_bounded(self):
        folder = self.root / "cache"
        cache = ocr_cache.PageCache(folder)
        try:
            with mock.patch.object(ocr_cache, "MAX_PAGES", 3):
                for index in range(10):
                    cache.save(folder / ("%064x.txt" % index), "", 500)
            self.assertEqual(cache.db.execute("SELECT entries FROM counters").fetchone()[0], 3)
            self.assertEqual(len(list(folder.glob("*.txt"))), 3)
        finally:
            cache.db.close()

    def test_cache_reconciles_orphan_after_crash(self):
        folder = self.root / "cache"
        folder.mkdir()
        (folder / ("a" * 64 + ".txt")).write_text("abc")
        cache = ocr_cache.PageCache(folder)
        try:
            self.assertEqual(cache.db.execute("SELECT total FROM counters").fetchone()[0], 3)
        finally:
            cache.db.close()

    def test_corrupt_optional_cache_does_not_lose_ocr_text(self):
        image = self.root / "image.png"
        image.write_bytes(b"png")
        folder = self.root / "cache"
        folder.mkdir()
        (folder / ".pages.sqlite").write_bytes(b"broken")
        with mock.patch.dict(
            os.environ, AI_SPACE_OCR_CACHE_DIR=str(folder), AI_SPACE_RESERVE_GIB=".01"
        ):
            result = extract.Extraction(image, ocr=True)
            result.save_ocr_cache(1, "识别成功正文")
        self.assertNotEqual(result.status, "failed")
        self.assertIn("ocr_cache_unavailable", result.issues)

    def test_snapshot_generation_mismatch_and_count_are_rejected(self):
        index = self.root / "index.json"
        payload = dict(version=3, items=[dict(path="/example", name="example", isDir=False)])
        indexer.atomic_json_write(index, payload)
        stream = Path(str(index) + ".ui.bin.gz")
        self.assertEqual(
            worker.inventory_metadata(stream, index)["generation"], payload["generation"]
        )
        data = json.loads(index.read_text())
        data["generation"] = "f" * 32
        index.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "版本不一致"):
            worker.inventory_metadata(stream, index)
        with gzip.open(stream, "rb") as source:
            raw = source.read()
        import struct

        length = struct.unpack("<I", raw[12:16])[0]
        meta = json.loads(raw[16 : 16 + length])
        meta["uiStreamItems"] = 2
        encoded = json.dumps(meta).encode()
        changed = raw[:12] + struct.pack("<I", len(encoded)) + encoded + raw[16 + length :]
        with gzip.open(stream, "wb") as target:
            target.write(changed)
        with self.assertRaisesRegex(ValueError, "count"):
            list(worker.iter_stream(stream))

    def test_large_snapshot_streams_with_bounded_python_heap(self):
        path = self.root / "index.json"
        with path.open("w") as output:
            output.write('{"version":3,"items":[')
            for index in range(1500):
                if index:
                    output.write(",")
                json.dump(
                    dict(path=str(index), content="正文" * 20000, source="filesystem"),
                    output,
                    ensure_ascii=False,
                )
            output.write("]}")
        tracemalloc.start()
        previous = indexer.previous_items(path)
        peak = tracemalloc.get_traced_memory()[1]
        tracemalloc.stop()
        try:
            self.assertEqual(len(previous), 1500)
            self.assertLess(peak, 16 * 1024 ** 2)
        finally:
            previous.close()

    def test_streaming_sequence_keeps_changes_and_small_first_order(self):
        data = RecordList(str(self.root))
        data.extend([dict(path="large", size=100), dict(path="small", size=1)])
        self.assertEqual(data[-1]["path"], "small")
        data[0] = dict(path="changed", size=5)
        data.sort_by_size()
        self.assertEqual([r["path"] for r in data], ["small", "changed"])
        data.close()

    def test_table_rows_coalesce_with_location_range(self):
        chunks = list(
            worker.chunks_from_blocks(
                (("工作表: Sheet1 行%d" % i, "数据%d" % i) for i in range(1, 101)), "表格", 256, 0
            )
        )
        self.assertLess(len(chunks), 10)
        self.assertIn(" — ", chunks[0][0])
        self.assertIn("行1", chunks[0][0])
        self.assertIn("数据100", chunks[-1][1])

    def test_large_failure_page_parses_only_visible_diagnostics_and_token_detects_change(self):
        path = self.root / "failures.sqlite"
        db = worker.open_db(str(path))
        count = 20000
        db.executemany(
            "INSERT INTO documents(id,path,name) VALUES(?,?,?)",
            ((i, "/source/%d" % i, "文件%d" % i) for i in range(1, count + 1)),
        )
        db.executemany(
            "INSERT INTO extraction_state(document_id,status,issues_json) VALUES(?,?,?)",
            ((i, "failed", '["Workbook is encrypted"]') for i in range(1, count + 1)),
        )
        failure_store.prepare(db)
        db.commit()
        with mock.patch.object(lib, "failure_category", wraps=lib.failure_category) as classify:
            begin = time.monotonic()
            answer = lib.failure_list(path, category="encrypted", offset=100)
            elapsed = time.monotonic() - begin
        self.assertEqual(answer["total"], count)
        self.assertEqual(len(answer["results"]), 50)
        self.assertLessEqual(classify.call_count, 50)
        self.assertLess(elapsed, 5)
        token = answer["selection_token"]
        db.execute("UPDATE documents SET name='改变' WHERE id=1")
        db.commit()
        with self.assertRaisesRegex(ValueError, "筛选结果已变化"):
            lib.set_ignored_matching(path, "encrypted", True, token, count)
        self.assertEqual(db.execute("SELECT count(*) FROM ignored_documents").fetchone()[0], 0)
        db.close()

    def test_long_diagnostic_keeps_persisted_failure_category(self):
        path = self.root / "long-failure.sqlite"
        db = worker.open_db(str(path))
        db.execute("INSERT INTO documents(id,path,name) VALUES(1,'/example','example')")
        db.execute(
            "INSERT INTO extraction_state(document_id,status,issues_json) VALUES(1,'failed',?)",
            (json.dumps(["Workbook is encrypted", "x" * 20000]),),
        )
        failure_store.prepare(db)
        db.commit()
        db.close()
        result = lib.failure_list(path, category="encrypted")
        self.assertEqual(result["total"], 1)
        self.assertEqual(result["results"][0]["failure_category"], "encrypted")

    def test_sqlite_failure_still_emits_final_json(self):
        folder = self.root / "managed"
        folder.mkdir()
        db = folder / "bad.sqlite"
        db.write_bytes(b"broken")
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "knowledge_jobs.py"),
                "--directory",
                str(folder),
                "--roots-json",
                "[]",
                "--retry-db",
                str(db),
                "--retry-documents-json",
                "[1]",
                "--reserve-gib",
                ".01",
            ],
            capture_output=True,
            text=True,
        )
        answer = json.loads(result.stdout.splitlines()[-1])
        self.assertTrue(answer["done"])
        self.assertTrue(answer["error"])
        self.assertNotIn("Traceback", result.stderr)

    def test_pipeline_lock_has_readable_error(self):
        folder = self.root / "managed"
        folder.mkdir()
        source = self.root / "source"
        source.mkdir()
        with (folder / ".pipeline.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPTS / "knowledge_jobs.py"),
                    "--directory",
                    str(folder),
                    "--roots-json",
                    json.dumps([str(source)]),
                    "--reserve-gib",
                    ".01",
                ],
                capture_output=True,
                text=True,
            )
        self.assertIn("另一个建库任务", json.loads(result.stdout.splitlines()[-1])["error"])

    def test_build_directory_is_user_content_and_bad_provider_is_isolated(self):
        source = self.root / "source"
        (source / "build").mkdir(parents=True)
        (source / "build" / "资料.txt").write_text("用户内容")
        output = self.root / "index.json"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "smart_space_indexer.py"),
                "--roots-json",
                json.dumps([str(source)]),
                "--output",
                str(output),
                "--metadata-only",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("资料.txt", {r["name"] for r in json.loads(output.read_text())["items"]})

    def test_sigkill_cache_recovery_preserves_active_cache(self):
        import signal
        import snapshot_io

        alive = ReuseMap(str(self.root))
        alive["active"] = {"body": "keep"}
        program = "import sys,time;sys.path.insert(0,sys.argv[1]);from snapshot_io import ReuseMap;c=ReuseMap(sys.argv[2]);c['a']={'v':1};print(c.temporary.name,flush=True);time.sleep(60)"
        child = subprocess.Popen(
            [sys.executable, "-c", program, str(SCRIPTS), str(self.root)],
            stdout=subprocess.PIPE,
            text=True,
        )
        abandoned = Path(child.stdout.readline().strip())
        try:
            child.kill()
            child.wait(timeout=3)
            snapshot_io._CLEANED.discard(str(self.root.resolve()))
            recovered = ReuseMap(str(self.root))
            try:
                self.assertFalse(abandoned.exists())
                self.assertTrue(Path(alive.temporary.name).exists())
                self.assertEqual(alive["active"]["body"], "keep")
            finally:
                recovered.close()
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            child.stdout.close()
            alive.close()

    def test_filename_index_preserves_middle_substrings(self):
        path = self.root / "names.sqlite"
        db = worker.open_db(str(path))
        name = "AnnualReport2026材料"
        db.execute("INSERT INTO documents(id,path,name) VALUES(1,?,?)", ("/example/" + name, name))
        db.execute(
            "INSERT INTO document_names_fts(rowid,name,document_id) VALUES(1,?,1)",
            (" ".join(c for c in name.casefold() if c.isalnum()),),
        )
        db.commit()
        db.close()
        for query in ("Report", "port2026", "材料"):
            answer = lib.search_one(path, query, limit=1)
            self.assertEqual(answer["results"][0]["name"], name)
            self.assertEqual(answer["results"][0]["match_scope"], "filename")

    def test_export_rejects_concurrent_build(self):
        path = self.root / "library.sqlite"
        db = worker.open_db(str(path))
        db.close()
        with worker.build_lock(str(path)):
            with self.assertRaisesRegex(RuntimeError, "already running"):
                lib.export_library(path, self.root / "busy.aikb", "test", 0.01, 1)
        self.assertFalse((self.root / "busy.aikb").exists())

    def test_text_malformed_json_arguments_finish_protocol(self):
        folder = self.root / "managed"
        source = self.root / "source"
        source.mkdir()
        for option, value in (("--exclude-roots-json", "{}"), ("--include-extensions-json", "42")):
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPTS / "knowledge_jobs.py"),
                    "--directory",
                    str(folder),
                    "--roots-json",
                    json.dumps([str(source)]),
                    option,
                    value,
                    "--reserve-gib",
                    ".01",
                ],
                capture_output=True,
                text=True,
            )
            answer = json.loads(result.stdout.splitlines()[-1])
            self.assertTrue(answer["done"])
            self.assertTrue(answer["error"])
            self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
