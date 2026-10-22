import contextlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import knowledge_library as lib
import smart_space_knowledge as worker


class KnowledgeLibraryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="aikb-test-")
        self.root = Path(self.temp.name)
        self.env = mock.patch.dict(
            os.environ,
            XDG_CONFIG_HOME=str(self.root / "config"),
            XDG_DATA_HOME=str(self.root / "data"),
        )
        self.env.start()
        self.doc = self.root / "originals/银行方案.txt"
        self.doc.parent.mkdir()
        self.doc.write_text("开头说明。\n" * 700 + "\n宁波银行 KMS 部署方案：统一密钥管理。\n" + "结束说明。\n" * 700)
        self.index = self.root / "index.json"
        self.index.write_text(
            json.dumps(
                dict(
                    items=[
                        dict(
                            path=str(self.doc),
                            root=str(self.doc.parent),
                            name=self.doc.name,
                            isDir=False,
                            category="documents",
                            content="",
                        )
                    ]
                ),
                ensure_ascii=False,
            )
        )
        self.db = self.root / "personal.sqlite"
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--no-ocr",
        )

    def tearDown(self):
        self.env.stop()
        self.temp.cleanup()

    def run_script(self, script, *args):
        out = subprocess.check_output(
            [sys.executable, str(SCRIPTS / script), *map(str, args)], text=True, timeout=30
        )
        return [json.loads(line) for line in out.splitlines() if line.strip()]

    def export(self):
        package = self.root / "shared.aikb"
        with open(os.devnull, "w") as out, contextlib.redirect_stdout(out):
            result = lib.export_library(self.db, package, "银行知识", 0.01, 1)
        return package, result

    def test_offline_search_and_hit_context(self):
        package, result = self.export()
        self.doc.unlink()
        lib.import_library(package, self.root / "external", "mount", 0.01, 1)
        answer = lib.search_libraries("宁波银行 KMS", db=str(self.root / "absent"), all_libraries=True)
        self.assertFalse(answer["timed_out"])
        self.assertTrue(answer["results"])
        hit = answer["results"][0]
        self.assertIn("部署方案", hit["snippet"])
        data = lib.fetch(package, hit["document_id"], hit["chunk_id"], limit=3)
        self.assertIn("统一密钥管理", "".join(r["content"] for r in data["results"]))
        self.assertGreater(data["results"][0]["ordinal"], 0)
        self.assertTrue(
            self.run_script(
                "knowledge_library.py", "fetch", "--reference", hit["reference"], "--limit", "3"
            )[-1]["results"]
        )
        self.assertFalse((self.root / "external").exists())
        self.assertFalse(result["manifest"]["originals"])

    def test_standalone_runtime_without_app_or_original(self):
        package, _ = self.export()
        self.doc.unlink()
        runtime = self.root / "agent-query.py"
        shutil.copy2(SCRIPTS / "knowledge_library.py", runtime)
        env = dict(os.environ, PYTHONPATH="", PYTHONNOUSERSITE="1")
        output = subprocess.check_output(
            [
                sys.executable,
                "-I",
                str(runtime),
                "--db",
                str(package),
                "search",
                "--query",
                "宁波银行 KMS",
            ],
            env=env,
            text=True,
        )
        self.assertTrue(json.loads(output)["results"])

    def test_duplicate_import_and_readonly_writer_guard(self):
        package, _ = self.export()
        result = lib.import_library(package, self.root / "imports", "copy", 0.01, 1)
        self.assertTrue(Path(result["library"]["path"]).is_file())
        self.assertTrue(
            lib.import_library(package, self.root / "imports", "copy", 0.01, 1)["duplicate"]
        )
        self.assertEqual(len(list((self.root / "imports").glob("*.aikb"))), 1)
        os.chmod(package, 0o600)
        with self.assertRaisesRegex(ValueError, "read-only"):
            worker.open_db(str(package))

    def test_disk_preflight_and_growth_guard(self):
        usage = shutil.disk_usage(self.root)
        with mock.patch.object(shutil, "disk_usage", return_value=usage._replace(free=100)):
            with self.assertRaises(lib.StorageLimitError):
                lib.export_library(self.db, self.root / "no.aikb", "x", 1, 32)
        with self.assertRaises(lib.StorageLimitError):
            lib.guard_storage(self.db, 0.01, 0.00001)
        with sqlite3.connect(self.db) as conn:
            conn.execute("CREATE TABLE quota_fixture(value BLOB)")
            conn.execute("INSERT INTO quota_fixture VALUES(zeroblob(12582912))")
        with self.assertRaises(lib.StorageLimitError):
            lib.export_library(self.db, self.root / "small.aikb", "x", 0.01, 0.01)
        self.assertFalse((self.root / "small.aikb").exists())

    def test_reject_damage_and_future_version(self):
        bad = self.root / "fake.aikb"
        bad.write_bytes(b"not sqlite")
        with self.assertRaises((ValueError, sqlite3.Error)):
            lib.import_library(bad, self.root / "imports", "copy", 0.01, 1)
        self.assertEqual(lib.registry()["libraries"], [])
        package, _ = self.export()
        os.chmod(package, 0o600)
        with sqlite3.connect(package) as db:
            manifest = lib.manifest(db, package)
            manifest["format_version"] = 999
            db.execute("UPDATE aikb_manifest SET json=?", (json.dumps(manifest),))
        with self.assertRaisesRegex(ValueError, "version"):
            lib.inspect_package(package)

    def test_failure_cleanup_and_no_clobber(self):
        with mock.patch.object(
            lib, "guard_storage", side_effect=lib.StorageLimitError("disk reserve")
        ):
            with self.assertRaises(lib.StorageLimitError):
                lib.export_library(self.db, self.root / "failed.aikb", "x", 0.01, 1)
        self.assertFalse((self.root / "failed.aikb").exists())
        self.assertFalse(list(self.root.glob(".*.partial-*")))
        package, _ = self.export()
        before = package.read_bytes()
        with self.assertRaises(FileExistsError):
            lib.export_library(self.db, package, "x", 0.01, 1)
        self.assertEqual(before, package.read_bytes())

    def test_export_includes_committed_wal(self):
        conn = sqlite3.connect(self.db)
        conn.execute("CREATE TABLE wal_proof(value TEXT)")
        conn.execute("INSERT INTO wal_proof VALUES('committed')")
        conn.commit()
        self.assertGreater(Path(str(self.db) + "-wal").stat().st_size, 0)
        package, _ = self.export()
        with sqlite3.connect(package) as read:
            self.assertEqual(read.execute("SELECT value FROM wal_proof").fetchone()[0], "committed")
        conn.close()

    def test_mounted_change_is_not_silent_empty_result(self):
        package, _ = self.export()
        lib.import_library(package, self.root / "imports", "mount", 0.01, 1)
        os.utime(package, ns=(package.stat().st_atime_ns, package.stat().st_mtime_ns + 10000000))
        answer = lib.search_libraries("银行", str(self.root / "none"), True)
        self.assertEqual(answer["libraries"][0]["status"], "unavailable")

    def test_storage_setting_and_legacy_override(self):
        config = lib.config_home() / "xikario"
        config.mkdir(parents=True)
        (config / "ai-space.ini").write_text(
            "[smartSpace]\nknowledgeDirectory=" + str(self.root / "chosen disk") + "\n"
        )
        self.assertEqual(
            lib.configured_db(), str(self.root / "chosen disk/smart-space-knowledge.sqlite")
        )
        with mock.patch.dict(os.environ, UKUI_FENCES_KNOWLEDGE_DB=str(self.db)):
            self.assertEqual(lib.configured_db(), str(self.db))

    def test_staged_build_resume_and_ocr_does_not_prune_text(self):
        folder = self.root / "managed"
        args = [
            "--directory",
            folder,
            "--roots-json",
            json.dumps([str(self.doc.parent)]),
            "--reserve-gib",
            ".01",
            "--max-gib",
            "1",
        ]
        lines = self.run_script("knowledge_jobs.py", *args, "--previous-db", self.db)
        self.assertTrue(lines[-1]["done"])
        self.assertEqual(
            [
                r["stage"]
                for r in lines
                if r.get("task") == "pipeline" and r.get("phase") == "start"
            ],
            ["manifest", "text"],
        )
        snapshot = json.loads((folder / "catalog/index.json").read_text())
        self.assertTrue(all(not r.get("content") for r in snapshot["items"]))
        with sqlite3.connect(folder / "smart-space-knowledge.sqlite") as conn:
            self.assertIn(
                "统一密钥管理", "".join(r[0] for r in conn.execute("SELECT content FROM chunks"))
            )
        self.assertEqual(lib.registry()["libraries"][0]["path"], str(self.db))
        second = self.run_script("knowledge_jobs.py", *args)
        done = [r for r in second if r.get("task") == "build" and r.get("done")][-1]
        self.assertEqual(done["reused"], 1)
        self.assertEqual(done["updated"], 0)
        self.run_script("knowledge_jobs.py", *args, "--ocr-only")
        self.assertEqual(lib.stats(folder / "smart-space-knowledge.sqlite")["documents"], 1)

    def test_real_ocr_child_limits_and_cpu_duty(self):
        from ocr_budget import run_ocr

        script = self.root / "probe.py"
        script.write_text(
            'import os,resource,json;print(json.dumps(dict(cores=len(os.sched_getaffinity(0)),memory=resource.getrlimit(resource.RLIMIT_AS)[0],threads=os.getenv("OMP_THREAD_LIMIT"))))'
        )
        output = self.root / "limits.json"
        with mock.patch.dict(
            os.environ, AI_SPACE_OCR_CPU_PERCENT="25", AI_SPACE_OCR_MEMORY_MB="512"
        ):
            with output.open("wb") as out:
                result = run_ocr([sys.executable, str(script)], stdout=out, timeout=5)
            self.assertEqual(result.returncode, 0)
            value = json.loads(output.read_text())
            self.assertEqual(value["cores"], 1)
            self.assertEqual(value["memory"], 512 * 1024 ** 2)
            self.assertEqual(value["threads"], "1")
            burner = self.root / "burn.py"
            burner.write_text(
                "import time\nt=time.process_time()\nwhile time.process_time()-t<.25:pass\n"
            )
            start = time.monotonic()
            result = run_ocr([sys.executable, str(burner)], stdout=subprocess.DEVNULL, timeout=8)
            self.assertEqual(result.returncode, 0)
            self.assertGreater(time.monotonic() - start, 0.65)

    def test_source_storage_collision_and_empty_manifest_cannot_destroy_library(self):
        command = [
            sys.executable,
            str(SCRIPTS / "knowledge_jobs.py"),
            "--directory",
            str(self.doc.parent),
            "--roots-json",
            json.dumps([str(self.doc.parent)]),
            "--reserve-gib",
            ".01",
        ]
        result = subprocess.run(command, stdout=subprocess.PIPE, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dedicated", json.loads(result.stdout.splitlines()[-1])["error"])
        folder = self.root / "managed"
        args = [
            "--directory",
            folder,
            "--roots-json",
            json.dumps([str(self.doc.parent)]),
            "--reserve-gib",
            ".01",
        ]
        self.run_script("knowledge_jobs.py", *args)
        before = lib.stats(folder / "smart-space-knowledge.sqlite")["chunks"]
        self.doc.unlink()
        result = subprocess.run(
            [sys.executable, str(SCRIPTS / "knowledge_jobs.py"), *map(str, args)],
            stdout=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Empty inventory", json.loads(result.stdout.splitlines()[-1])["error"])
        self.assertEqual(lib.stats(folder / "smart-space-knowledge.sqlite")["chunks"], before)

    def test_changed_package_identity_conflict_and_unmount_keeps_data(self):
        package, _ = self.export()
        entry = lib.import_library(package, self.root / "imports", "copy", 0.01, 1)["library"]
        os.chmod(package, 0o600)
        with sqlite3.connect(package) as db:
            db.execute("UPDATE chunks SET content='changed content' WHERE ordinal=0")
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            lib.import_library(package, self.root / "imports", "copy", 0.01, 1)
        self.run_script(
            "knowledge_library.py",
            "unmount",
            "--library-id",
            entry["library_id"],
            "--version",
            entry["version"],
        )
        self.assertEqual(lib.registry()["libraries"], [])
        self.assertTrue(Path(entry["path"]).is_file())

    def test_text_reuse_survives_ocr_policy_change(self):
        lines = self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--ocr",
        )
        self.assertEqual(lines[-1]["reused"], 1)
        self.assertEqual(lines[-1]["updated"], 0)

    def test_ocr_cache_resume_version_and_bound(self):
        import smart_space_extract as extract

        cache = self.root / "ocr-cache"
        with mock.patch.dict(os.environ, AI_SPACE_OCR_CACHE_DIR=str(cache)), mock.patch.object(
            extract, "OCR_CACHE_LIMIT_BYTES", 20
        ):
            first = extract.Extraction(self.doc, ocr=True)
            with mock.patch.object(first, "_ocr_page", return_value="page1") as producer:
                self.assertEqual(first.ocr_page(1, self.root), "page1")
                producer.assert_called_once()
            resumed = extract.Extraction(self.doc, ocr=True)
            with mock.patch.object(
                resumed, "_ocr_page", side_effect=AssertionError("must reuse saved page")
            ):
                self.assertEqual(resumed.ocr_page(1, self.root), "page1")
            for page in range(2, 8):
                resumed.save_ocr_cache(page, "1234567")
            self.assertLessEqual(sum(p.stat().st_size for p in cache.glob("*.txt")), 20)
            self.doc.write_text("new file revision")
            with self.assertRaisesRegex(RuntimeError, "source changed"):
                resumed.save_ocr_cache(2, "stale")
            newest = extract.Extraction(self.doc, ocr=True)
            with mock.patch.object(newest, "_ocr_page", return_value="new page") as producer:
                self.assertEqual(newest.ocr_page(1, self.root), "new page")
                producer.assert_called_once()

    def test_low_memory_prevents_ocr_spawn(self):
        import ocr_budget

        with mock.patch.object(ocr_budget, "available_memory", return_value=0), mock.patch.object(
            subprocess, "Popen"
        ) as spawn:
            with self.assertRaises(ocr_budget.ResourcePressureError):
                ocr_budget.run_ocr([sys.executable, "-c", "pass"])
            spawn.assert_not_called()

    def test_cancel_ocr_reaps_stopped_child(self):
        import signal

        child = self.root / "slow.py"
        pidfile = self.root / "child.pid"
        child.write_text(
            "import os,time\nfrom pathlib import Path\nPath("
            + repr(str(pidfile))
            + ").write_text(str(os.getpid()))\ntime.sleep(30)\n"
        )
        code = (
            "import sys;sys.path.insert(0,"
            + repr(str(SCRIPTS))
            + ");from ocr_budget import run_ocr;run_ocr([sys.executable,"
            + repr(str(child))
            + "],timeout=40)"
        )
        process = subprocess.Popen(
            [sys.executable, "-c", code],
            start_new_session=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            deadline = time.monotonic() + 5
            while not pidfile.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertTrue(pidfile.exists())
            child_pid = int(pidfile.read_text())
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=8)
            deadline = time.monotonic() + 3
            while Path("/proc/" + str(child_pid)).exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(Path("/proc/" + str(child_pid)).exists())
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()

    def test_optional_ocr_runs_only_after_text_and_records_state(self):
        image = self.doc.parent / "扫描.png"
        image.write_bytes(b"\x89PNG\r\n\x1a\n")
        tools = self.root / "tools"
        tools.mkdir()
        fake = tools / "tesseract"
        fake.write_text(
            "#!"
            + sys.executable
            + '\nimport sys\nprint("chi_sim\\neng" if "--list-langs" in sys.argv else "银行扫描 OCR 正文")\n'
        )
        fake.chmod(0o755)
        with mock.patch.dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"]):
            result = self.run_script(
                "knowledge_jobs.py",
                "--directory",
                self.root / "managed",
                "--roots-json",
                json.dumps([str(self.doc.parent)]),
                "--ocr",
                "--reserve-gib",
                ".01",
            )
        self.assertEqual(
            [
                r["stage"]
                for r in result
                if r.get("task") == "pipeline" and r.get("phase") == "start"
            ],
            ["manifest", "text", "ocr"],
        )
        self.assertEqual(result[-1]["extractionStates"]["complete"], 2)
        with sqlite3.connect(self.root / "managed/smart-space-knowledge.sqlite") as db:
            self.assertTrue(
                db.execute("SELECT 1 FROM chunks WHERE content LIKE '%银行扫描 OCR 正文%'").fetchone()
            )

    def test_versions_switch_without_deleting_previous_and_keep_old_reference(self):
        first, _ = self.export()
        one = lib.import_library(first, self.root / "imports", "copy", 0.01, 1)["library"]
        second = self.root / "new-version.aikb"
        with open(os.devnull, "w") as out, contextlib.redirect_stdout(out):
            lib.export_library(self.db, second, "新版本", 0.01, 1)
        two = lib.import_library(second, self.root / "imports", "copy", 0.01, 1)["library"]
        self.assertEqual(one["library_id"], two["library_id"])
        self.assertNotEqual(one["version"], two["version"])
        selected = lib.selected_paths(str(self.root / "absent"), True)
        self.assertEqual(len(selected), 1)
        self.assertEqual(selected[0]["version"], two["version"])
        self.assertTrue(Path(one["path"]).exists())
        lib.enable_version(one["library_id"], one["version"])
        self.assertEqual(
            lib.selected_paths(str(self.root / "absent"), True)[0]["version"], one["version"]
        )

    def test_broken_personal_library_does_not_block_shared_search(self):
        package, _ = self.export()
        lib.import_library(package, self.root / "imports", "mount", 0.01, 1)
        broken = self.root / "broken.sqlite"
        broken.write_bytes(b"bad database")
        result = lib.search_libraries("宁波银行 KMS", str(broken), True)
        self.assertTrue(result["results"])
        self.assertTrue(result["incomplete"])
        self.assertEqual(result["libraries"][0]["status"], "error")

    def test_filename_fast_path_and_explicit_body_search(self):
        named = lib.search_one(self.db, "银行方案", limit=1)
        self.assertTrue(named["results"])
        self.assertFalse(named["body_searched"])
        self.assertEqual(named["results"][0]["match_scope"], "filename")
        body = lib.search_one(self.db, "宁波银行 KMS", limit=1, body_only=True)
        self.assertTrue(body["body_searched"])
        self.assertEqual(body["results"][0]["match_scope"], "body")

    def test_qsettings_unicode_storage_path_decode(self):
        self.assertEqual(
            lib.qt_ini_string(r'"/data/\x77e5\x8bc6\x5e93/\xd83d\xde00"'), "/data/知识库/😀"
        )
        self.assertEqual(
            lib.qt_ini_string(r"C:\\Users\\Test"), "C:\\Users\\Test".replace("\\", "\\")
        )

    def test_metadata_without_body_is_searchable_and_not_false_evidence(self):
        self.doc.write_text("")
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--no-ocr",
        )
        result = lib.search_one(self.db, "银行方案", limit=1)
        self.assertFalse(result["results"][0]["hasBody"])
        hit = result["results"][0]
        value = lib.fetch(self.db, hit["document_id"], hit["chunk_id"])
        self.assertEqual(value["results"], [])
        self.assertEqual(value["extractionStatus"], "no_text")

    def test_mismatched_inventory_stops_without_touching_existing_body(self):
        folder = self.root / "managed"
        args = [
            "--directory",
            folder,
            "--roots-json",
            json.dumps([str(self.doc.parent)]),
            "--reserve-gib",
            ".01",
        ]
        self.run_script("knowledge_jobs.py", *args)
        (folder / "catalog/index.json").write_text("not valid JSON; stream is authoritative")
        db = folder / "smart-space-knowledge.sqlite"
        before = db.read_bytes()
        result = subprocess.run(
            [sys.executable, str(SCRIPTS / "knowledge_jobs.py"), *map(str, args), "--ocr-only"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(json.loads(result.stdout.splitlines()[-1])["error"])
        self.assertEqual(before, db.read_bytes())

    def test_explicit_scan_then_text_stage(self):
        folder = self.root / "separate"
        args = [
            "--directory",
            folder,
            "--roots-json",
            json.dumps([str(self.doc.parent)]),
            "--reserve-gib",
            ".01",
        ]
        result = self.run_script("knowledge_jobs.py", *args, "--manifest-only")
        self.assertEqual(result[-1]["phase"], "manifest-complete")
        self.assertFalse((folder / "smart-space-knowledge.sqlite").exists())
        before = (folder / "catalog/index.json.ui.bin.gz").stat().st_mtime_ns
        result = self.run_script("knowledge_jobs.py", *args, "--text-only")
        self.assertEqual(result[-1]["documents"], 1)
        self.assertEqual((folder / "catalog/index.json.ui.bin.gz").stat().st_mtime_ns, before)
        self.assertFalse(any(row.get("stage") == "manifest" for row in result))
        other = self.root / "other-source"
        other.mkdir()
        changed = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "knowledge_jobs.py"),
                "--directory",
                str(folder),
                "--roots-json",
                json.dumps([str(other)]),
                "--text-only",
                "--reserve-gib",
                ".01",
            ],
            text=True,
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(changed.returncode, 0)
        self.assertIn("资料目录已改变", changed.stdout)
        self.assertEqual(lib.stats(folder / "smart-space-knowledge.sqlite")["documents"], 1)

    def test_text_stage_requires_manifest(self):
        folder = self.root / "missing-scan"
        value = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "knowledge_jobs.py"),
                "--directory",
                str(folder),
                "--roots-json",
                json.dumps([str(self.doc.parent)]),
                "--text-only",
                "--reserve-gib",
                ".01",
            ],
            text=True,
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(value.returncode, 0)
        self.assertIn("第 1 步", value.stdout)
        self.assertFalse((folder / "smart-space-knowledge.sqlite").exists())

    def test_details_count_only_explicit_ocr_markers(self):
        with sqlite3.connect(self.db) as db:
            db.execute(
                "UPDATE extraction_state SET status='partial',issues_json=?",
                (
                    json.dumps(
                        [
                            "page_needs_ocr:2",
                            "page_needs_ocr:5",
                            "page_needs_ocr:5",
                            "empty_ocr_page:7",
                        ]
                    ),
                ),
            )
        result = lib.stats(self.db, True)
        self.assertEqual(result["ocrPending"]["documents"], 1)
        self.assertEqual(result["ocrPending"]["pdfPages"], 2)
        self.assertEqual(result["ocrPending"]["emptyOcrPages"], 1)
        with sqlite3.connect(self.db) as db:
            db.execute("UPDATE extraction_state SET status='no_text',issues_json='[]'")
        self.assertEqual(lib.stats(self.db, True)["ocrPending"]["documents"], 0)

    def test_review_unreadable_subdirectory_stops_pipeline_without_pruning(self):
        hidden = self.doc.parent / "locked"
        hidden.mkdir()
        (hidden / "keep.txt").write_text("不可丢失的正文")
        folder = self.root / "managed"
        args = [
            "--directory",
            folder,
            "--roots-json",
            json.dumps([str(self.doc.parent)]),
            "--reserve-gib",
            ".01",
        ]
        self.run_script("knowledge_jobs.py", *args)
        database = folder / "smart-space-knowledge.sqlite"
        before = lib.stats(database)["documents"]
        hidden.chmod(0)
        try:
            result = subprocess.run(
                [sys.executable, str(SCRIPTS / "knowledge_jobs.py"), *map(str, args)],
                stdout=subprocess.PIPE,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("incomplete", result.stdout)
            self.assertEqual(lib.stats(database)["documents"], before)
        finally:
            hidden.chmod(0o700)

    def test_review_absence_preserves_body_and_marks_unseen(self):
        self.index.write_text(json.dumps(dict(items=[])))
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--no-ocr",
        )
        result = lib.search_one(self.db, "银行方案", 1)["results"][0]
        self.assertEqual(result["sourceAvailability"], "unseen")
        self.assertTrue(lib.fetch(self.db, result["document_id"])["results"])

    def test_review_partial_and_no_text_ocr_reused_by_text_pass(self):
        source = self.doc.with_suffix(".pdf")
        source.write_bytes(b"cached OCR fixture")
        item = dict(
            path=str(source),
            root=str(source.parent),
            name=source.name,
            isDir=False,
            category="documents",
            content="",
        )
        self.index.write_text(json.dumps(dict(items=[item])))
        args = worker.parser().parse_args(
            ["build", "--db", str(self.db), "--index-json", str(self.index), "--no-ocr"]
        )
        for state in ("partial", "no_text"):
            with sqlite3.connect(self.db) as db:
                db.execute(
                    "UPDATE documents SET path=?,name=?,suffix=?", (str(source), source.name, "pdf")
                )
                db.execute(
                    "UPDATE extraction_state SET status=?,source_signature=?",
                    (state, worker.source_signature(item, args, ocr=True)),
                )
                before = db.execute("SELECT content FROM chunks").fetchall()
            self.run_script(
                "smart_space_knowledge.py",
                "build",
                "--db",
                self.db,
                "--index-json",
                self.index,
                "--no-ocr",
            )
            with sqlite3.connect(self.db) as db:
                self.assertEqual(before, db.execute("SELECT content FROM chunks").fetchall())
                self.assertEqual(
                    state, db.execute("SELECT status FROM extraction_state").fetchone()[0]
                )

    def test_review_ocr_wall_budget_accounts_for_duty_cycle(self):
        from ocr_budget import run_ocr

        with mock.patch.dict(os.environ, AI_SPACE_OCR_CPU_PERCENT="25"):
            value = run_ocr([sys.executable, "-c", "import time;time.sleep(.35)"], timeout=0.2)
        self.assertEqual(value.returncode, 0)

    def test_review_reference_survives_unrelated_write_and_move_but_not_document_change(self):
        hit = lib.search_one(self.db, "银行方案", 1)["results"][0]
        with sqlite3.connect(self.db) as db:
            db.execute("INSERT OR REPLACE INTO meta VALUES('other','changed')")
            db.commit()
            db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        moved = self.root / "moved.sqlite"
        shutil.copy2(self.db, moved)
        result = self.run_script(
            "knowledge_library.py", "--db", moved, "fetch", "--reference", hit["reference"]
        )[-1]
        self.assertTrue(result["results"])
        self.doc.write_text("文档已替换为全新正文")
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            moved,
            "--index-json",
            self.index,
            "--no-ocr",
        )
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "knowledge_library.py"),
                "--db",
                str(moved),
                "fetch",
                "--reference",
                hit["reference"],
            ],
            text=True,
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(result.returncode, 0)

    def test_review_bad_registry_keeps_personal_search_and_reports_coverage(self):
        lib.registry_path().parent.mkdir(parents=True, exist_ok=True)
        lib.registry_path().write_text("{broken")
        with mock.patch.dict(os.environ, AI_SPACE_KNOWLEDGE_DB=str(self.db)):
            value = lib.search_libraries("银行方案", all_libraries=True)
        self.assertTrue(value["results"])
        self.assertTrue(value["incomplete"])

    def test_review_missing_library_and_unsupported_language_are_not_clean_empty(self):
        value = lib.search_libraries("客户", db=str(self.root / "absent.sqlite"))
        self.assertTrue(value["incomplete"])
        self.assertTrue(value["error"])
        value = lib.search_libraries("каталог", db=str(self.db))
        self.assertTrue(value["incomplete"])
        self.assertEqual(value["libraries"][0]["status"], "unsupported_language")

    def test_review_unmount_reimport_reuses_verified_copy(self):
        package, info = self.export()
        folder = self.root / "copies"
        with open(os.devnull, "w") as sink, contextlib.redirect_stdout(sink):
            first = lib.import_library(package, folder, "copy", 0.01, 1)
        with lib.registry_write() as data:
            data["libraries"] = []
        with open(os.devnull, "w") as sink, contextlib.redirect_stdout(sink):
            second = lib.import_library(package, folder, "copy", 0.01, 1)
        self.assertEqual(first["library"]["path"], second["library"]["path"])
        self.assertEqual(len(list(folder.glob("*.aikb"))), 1)

    def test_review_fetch_budget_and_untrusted_markers(self):
        value = lib.fetch(self.db, 1, limit=30, max_chars=2000)
        self.assertLessEqual(sum(len(row["content"]) for row in value["results"]), 2000)
        self.assertEqual(value["content_trust"], "untrusted_document")
        self.assertTrue(value["hasMore"] or value["results"][0]["content_truncated"])

    def test_review_direct_builder_rejects_incomplete_inventory(self):
        self.index.write_text(json.dumps(dict(errors=[dict(path="unreadable")], items=[])))
        value = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "smart_space_knowledge.py"),
                "build",
                "--db",
                str(self.db),
                "--index-json",
                str(self.index),
                "--no-ocr",
            ],
            text=True,
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(value.returncode, 0)
        self.assertEqual(lib.stats(self.db)["documents"], 1)

    def test_review_id_only_fetch_requires_explicit_library(self):
        value = subprocess.run(
            [sys.executable, str(SCRIPTS / "knowledge_library.py"), "fetch", "--document-id", "1"],
            text=True,
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(value.returncode, 0)
        self.assertIn("reference", value.stdout)

    def test_review_legacy_query_budget_is_not_query_text(self):
        script = SCRIPTS.parent / "skills/ai-space-index-query/scripts/query.py"
        value = subprocess.check_output(
            [
                sys.executable,
                str(script),
                "--db",
                str(self.db),
                "银行方案",
                "--json",
                "--budget-ms",
                "5000",
            ],
            text=True,
        )
        self.assertEqual(json.loads(value)["query"], "银行方案")

    def test_review_provider_requires_approval_and_confines_inheritance(self):
        import smart_space_indexer as indexer
        from provider_policy import load_policy

        config = self.root / "provider.json"
        marker = self.root / "executed"
        config.write_text(
            json.dumps(
                dict(
                    providers=[
                        dict(
                            type="command",
                            program=sys.executable,
                            arguments=[
                                "-c",
                                "from pathlib import Path;Path(%r).write_text('bad')" % str(marker),
                            ],
                        )
                    ]
                )
            )
        )
        errors = []
        indexer.collect_provider_items(config, [], errors)
        self.assertTrue(errors)
        self.assertFalse(marker.exists())
        config.write_text(json.dumps(dict(providers=[dict(inheritFrom="../outside.json")])))
        with self.assertRaises(ValueError):
            load_policy(config)

    def test_review_provider_change_invalidates_approval_and_roots_are_opt_in(self):
        import smart_space_indexer as indexer
        from provider_policy import load_policy

        config = self.root / "provider.json"
        config.write_text(
            json.dumps(
                dict(
                    providers=[
                        dict(
                            type="command", program=sys.executable, arguments=["-c", 'print("[]")']
                        )
                    ]
                )
            )
        )
        approved = load_policy(config)[1]
        config.write_text(
            json.dumps(
                dict(
                    providers=[
                        dict(
                            type="command",
                            program=sys.executable,
                            arguments=["-c", 'raise RuntimeError("must not execute")'],
                        )
                    ]
                )
            )
        )
        with mock.patch.dict(os.environ, AI_SPACE_PROVIDER_APPROVAL=approved):
            errors = []
            indexer.collect_provider_items(config, [], errors)
        self.assertIn("approval", errors[0]["error"])
        self.assertEqual(indexer.provider_request({}, [self.doc.parent])["roots"], [])

    def test_final_scoped_build_keeps_other_roots_available(self):
        other = self.root / "other/kept.txt"
        other.parent.mkdir()
        other.write_text("其他目录应保持可用")
        first = json.loads(self.index.read_text())["items"]
        self.index.write_text(
            json.dumps(
                dict(
                    items=first
                    + [dict(path=str(other), root=str(other.parent), name=other.name, isDir=False)]
                )
            )
        )
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--no-ocr",
        )
        self.index.write_text(json.dumps(dict(items=[])))
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--no-ocr",
            "--include-root",
            self.doc.parent,
        )
        with sqlite3.connect(self.db) as conn:
            availability = dict(
                conn.execute(
                    "SELECT path,status FROM documents JOIN document_availability ON documents.id=document_id"
                )
            )
        self.assertEqual(availability[str(self.doc)], "unseen")
        self.assertEqual(availability[str(other)], "present")

    def test_final_malformed_registry_entry_keeps_personal_library(self):
        path = lib.registry_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(dict(version=1, libraries=[dict(path=str(self.db), read_only=True)]))
        )
        answer = lib.search_libraries("银行方案", db=str(self.db), all_libraries=True)
        self.assertTrue(answer["results"])
        self.assertTrue(answer["incomplete"])
        self.assertEqual(answer["libraries"][-1]["status"], "unavailable")

    def test_final_reference_paging_never_switches_to_personal_same_id(self):
        package, _ = self.export()
        lib.import_library(package, self.root / "mount", "mount", 0.01, 1)
        hit = lib.search_one(package, "银行方案", 1)["results"][0]
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE chunks SET content='PERSONAL_WRONG_DOCUMENT'")
            conn.execute("UPDATE documents SET content_hash='personal-new-content'")
        first = self.run_script(
            "knowledge_library.py",
            "--db",
            self.db,
            "fetch",
            "--reference",
            hit["reference"],
            "--limit",
            "1",
        )[-1]
        self.assertTrue(first["hasMore"])
        page = self.run_script(
            "knowledge_library.py",
            "--db",
            self.db,
            "fetch",
            "--reference",
            hit["reference"],
            "--offset",
            first["nextOffset"],
            "--limit",
            "1",
        )[-1]
        expected = lib.fetch(package, hit["document_id"], offset=first["nextOffset"], limit=1)
        self.assertEqual(page["results"], expected["results"])
        self.assertNotIn("PERSONAL_WRONG_DOCUMENT", page["results"][0]["content"])

    def test_final_old_inventory_rejected_after_ocr_roots_or_filters_change(self):
        folder = self.root / "managed"
        base = [
            "--directory",
            folder,
            "--roots-json",
            json.dumps([str(self.doc.parent)]),
            "--reserve-gib",
            ".01",
        ]
        self.run_script("knowledge_jobs.py", *base)
        db = folder / "smart-space-knowledge.sqlite"
        before = db.stat().st_mtime_ns
        other = self.root / "replacement"
        other.mkdir()
        commands = [
            ["--roots-json", json.dumps([str(other)]), "--ocr-only"],
            ["--include-extensions-json", '["txt"]', "--ocr-only"],
            ["--exclude-roots-json", json.dumps([str(self.doc.parent)]), "--text-only"],
        ]
        for changed in commands:
            value = subprocess.run(
                [sys.executable, str(SCRIPTS / "knowledge_jobs.py"), *map(str, base), *changed],
                text=True,
                stdout=subprocess.PIPE,
            )
            self.assertNotEqual(value.returncode, 0)
            self.assertIn("已改变", value.stdout)
            self.assertEqual(db.stat().st_mtime_ns, before)

    def test_final_provider_command_and_dbus_output_are_bounded(self):
        import smart_space_indexer as indexer

        with self.assertRaises((RuntimeError, ValueError)):
            from provider_policy import command_identity

            program, identity = command_identity(sys.executable)
            indexer.run_provider(
                dict(
                    type="command",
                    program=program,
                    _programIdentity=identity,
                    arguments=["-c", 'print("x"*(5*1024*1024))'],
                ),
                [],
            )
        tools = self.root / "bin"
        tools.mkdir()
        bus = tools / "gdbus"
        bus.write_text("#!" + sys.executable + '\nprint("x"*(5*1024*1024))\n')
        bus.chmod(0o755)
        with mock.patch.dict(os.environ, PATH=str(tools) + os.pathsep + os.environ.get("PATH", "")):
            with self.assertRaises((RuntimeError, ValueError)):
                indexer.run_provider(
                    dict(
                        type="dbus",
                        service="org.example.Provider",
                        objectPath="/provider",
                        method="org.example.Provider.query",
                    ),
                    [],
                )

    def test_final_http_provider_rejects_redirect_and_oversized_response(self):
        import http.server, threading
        import smart_space_indexer as indexer

        destinations = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
                if self.path == "/redirect":
                    self.send_response(302)
                    self.send_header("Location", "/unapproved")
                    self.end_headers()
                else:
                    self.send_response(200)
                    self.end_headers()
                    try:
                        self.wfile.write(b"x" * (4 * 1024 * 1024 + 1))
                    except (BrokenPipeError, ConnectionResetError):
                        pass

            def do_GET(self):
                destinations.append(self.path)
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"[]")

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = "http://127.0.0.1:" + str(server.server_port)
            with self.assertRaisesRegex(ValueError, "redirect"):
                indexer.run_provider(dict(type="http", url=url + "/redirect"), [])
            self.assertFalse(destinations)
            with self.assertRaisesRegex(ValueError, "4 MiB"):
                indexer.run_provider(dict(type="http", url=url + "/large"), [])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_failure_ignore_restore_preserves_body_and_skips_even_force(self):
        with sqlite3.connect(self.db) as conn:
            conn.execute(
                "UPDATE extraction_state SET status='failed',issues_json='[\"fixture failure\"]'"
            )
            before = conn.execute("SELECT content FROM chunks ORDER BY id").fetchall()
        self.assertEqual(lib.failure_list(self.db)["total"], 1)
        lib.set_ignored(self.db, [1], True)
        self.assertEqual(lib.failure_list(self.db)["total"], 0)
        self.assertEqual(lib.failure_list(self.db, True)["total"], 1)
        self.assertEqual(lib.stats(self.db, True)["ignoredDocuments"], 1)
        self.assertEqual(lib.stats(self.db, True)["extractionStates"]["failed"], 1)
        self.doc.write_text("原件已改变，但忽略期间不能自动重提取")
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--no-ocr",
            "--force",
        )
        with sqlite3.connect(self.db) as conn:
            self.assertEqual(
                before, conn.execute("SELECT content FROM chunks ORDER BY id").fetchall()
            )
        lib.set_ignored(self.db, [1], False)
        self.assertEqual(lib.failure_list(self.db)["total"], 1)
        self.assertEqual(lib.stats(self.db, True)["ignoredDocuments"], 0)

    def test_failure_policy_rejects_readonly_stale_ids_and_active_writer(self):
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE extraction_state SET status='failed'")
        package, _ = self.export()
        before = package.read_bytes()
        with self.assertRaisesRegex(ValueError, "read-only"):
            lib.set_ignored(package, [1], True)
        self.assertEqual(package.read_bytes(), before)
        with self.assertRaises(ValueError):
            lib.set_ignored(self.db, [999999], True)
        with worker.build_lock(str(self.db)):
            with self.assertRaisesRegex(RuntimeError, "running"):
                lib.set_ignored(self.db, [1], True)
        with contextlib.closing(lib.open_readonly(self.db)) as conn:
            self.assertFalse(lib.ignored_ids(conn))

    def test_failure_list_paginates_all_records_and_keeps_full_reasons(self):
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE extraction_state SET status='failed'")
            for i in range(1, 56):
                ident = conn.execute(
                    "INSERT INTO documents(path,name) VALUES(?,?)",
                    (str(self.root / ("failed-%d.txt" % i)), "duplicate.txt"),
                ).lastrowid
                conn.execute(
                    "INSERT INTO extraction_state(document_id,status,issues_json) VALUES(?,'missing',?)",
                    (ident, json.dumps(["找不到来源文件 " + str(i)])),
                )
        first = lib.failure_list(self.db, limit=50)
        second = lib.failure_list(self.db, offset=50, limit=50)
        self.assertEqual(first["total"], 56)
        self.assertEqual(len(first["results"]), 50)
        self.assertEqual(len(second["results"]), 6)
        self.assertNotEqual(first["results"][1]["path"], first["results"][2]["path"])
        self.assertIn("找不到来源文件", second["results"][0]["reason"])

    def seed_category_failures(self, encrypted=75):
        with sqlite3.connect(self.db) as conn:
            conn.execute(
                "UPDATE extraction_state SET status='failed',issues_json='[\"Workbook is encrypted\"]'"
            )
            ids = [1]
            for number in range(1, encrypted + 1):
                ident = conn.execute(
                    "INSERT INTO documents(path,name) VALUES(?,?)",
                    (str(self.root / ("failure-%d.xls" % number)), "failure-%d.xls" % number),
                ).lastrowid
                issue = (
                    "Workbook is encrypted"
                    if number < encrypted
                    else "unsupported container: unknown"
                )
                conn.execute(
                    "INSERT INTO extraction_state(document_id,status,issues_json) VALUES(?,'failed',?)",
                    (ident, json.dumps([issue, "previous_chunks_retained"])),
                )
                if number < encrypted:
                    ids.append(ident)
        return ids

    def test_failure_classification_counts_and_filter_before_pagination(self):
        ids = self.seed_category_failures(75)
        before = self.db.read_bytes()
        first = lib.failure_list(self.db, category="encrypted")
        second = lib.failure_list(self.db, offset=50, category="encrypted")
        self.assertEqual(first["total"], 75)
        self.assertEqual(len(first["results"]), 50)
        self.assertEqual(len(second["results"]), 25)
        self.assertEqual(first["selection_token"], second["selection_token"])
        self.assertEqual(first["selection_ids"], ids)
        counts = {row["id"]: row["count"] for row in first["categories"]}
        self.assertEqual((counts["all"], counts["encrypted"], counts["unsupported"]), (76, 75, 1))
        self.assertTrue(
            all(
                row["failure_category"] == "encrypted"
                for row in first["results"] + second["results"]
            )
        )
        self.assertEqual(lib.failure_list(self.db, category="unsupported")["total"], 1)
        self.assertEqual(before, self.db.read_bytes())

    def test_failure_classification_preserves_distinct_causes_and_missing_priority(self):
        cases = [
            ("failed", "Workbook is encrypted", "encrypted"),
            ("failed", "旧版演示文稿已加密", "encrypted"),
            ("missing", "Workbook is encrypted", "missing"),
            ("failed", "[Errno 2] No such file or directory: '/tmp/encrypted.xls'", "missing"),
            ("failed", "Permission denied: '/tmp/encrypted.xls'", "permissions"),
            ("failed", "unsupported container: ole-unknown", "unsupported"),
            ("failed", "Workbook corruption: seen[3] == 4", "corrupt"),
            ("failed", "pdftotext failed (1)", "extraction"),
            ("failed", "tesseract failed (1)", "ocr"),
            ("failed", "tesseract not installed", "tools"),
            ("failed", "PDF page rendering failed: 2", "ocr"),
            ("failed", "'gb18030' codec can't decode byte 0xa9", "encoding"),
            ("failed", "处理超时", "timeout"),
            ("failed", "MemoryError", "resources"),
            ("failed", "new unrecognized parser error", "other"),
        ]
        for status, issue, category in cases:
            with self.subTest(issue=issue):
                self.assertEqual(
                    lib.failure_category(status, [issue, "previous_chunks_retained"]), category
                )

    def test_bulk_ignore_restore_over_200_matches_preserves_body_and_other_category(self):
        ids = self.seed_category_failures(215)
        body = lib.fetch(self.db, 1)["results"]
        chosen = lib.failure_list(self.db, category="encrypted")
        self.assertEqual(chosen["selection_ids"], [])
        args = [
            "failure-policy",
            "--all-matching",
            "--category",
            "encrypted",
            "--expected-selection",
            chosen["selection_token"],
            "--expected-count",
            "215",
        ]
        result = self.run_script(
            "knowledge_library.py", "--db", self.db, *args, "--action", "ignore"
        )[-1]
        self.assertEqual(result["changed"], 215)
        self.assertEqual(lib.failure_list(self.db)["total"], 1)
        ignored = lib.failure_list(self.db, True, category="encrypted")
        self.assertEqual(ignored["total"], 215)
        self.assertEqual(body, lib.fetch(self.db, 1)["results"])
        self.assertEqual(lib.stats(self.db)["extractionStates"]["failed"], 216)
        restored = self.run_script(
            "knowledge_library.py",
            "--db",
            self.db,
            "failure-policy",
            "--all-matching",
            "--category",
            "encrypted",
            "--expected-selection",
            ignored["selection_token"],
            "--expected-count",
            "215",
            "--action",
            "restore",
        )[-1]
        self.assertEqual(restored["changed"], 215)
        self.assertEqual(lib.failure_list(self.db)["total"], 216)
        self.assertEqual(lib.failure_list(self.db, True)["total"], 0)

    def test_bulk_selection_rejects_changed_results_and_mode_without_writes(self):
        ids = self.seed_category_failures(3)
        chosen = lib.failure_list(self.db, category="encrypted")
        with sqlite3.connect(self.db) as conn:
            # Keep the count identical while replacing which files match.
            conn.execute(
                "UPDATE extraction_state SET issues_json='[\"unsupported container: unknown\"]' WHERE document_id=1"
            )
            conn.execute(
                "UPDATE extraction_state SET issues_json='[\"Workbook is encrypted\"]' WHERE document_id=4"
            )
        with self.assertRaisesRegex(ValueError, "已变化"):
            lib.set_ignored_matching(self.db, "encrypted", True, chosen["selection_token"], 3)
        self.assertEqual(lib.failure_list(self.db, True)["total"], 0)
        refreshed = lib.failure_list(self.db, category="encrypted")
        with self.assertRaisesRegex(ValueError, "已变化"):
            lib.set_ignored_matching(self.db, "encrypted", False, refreshed["selection_token"], 3)
        with self.assertRaisesRegex(ValueError, "请先全选"):
            lib.set_ignored_matching(self.db, "encrypted", True, "", 3)
        with self.assertRaisesRegex(ValueError, "未知"):
            lib.failure_list(self.db, category="invented")

    def test_bulk_policy_atomic_rollback_readonly_and_writer_lock(self):
        self.seed_category_failures(3)
        chosen = lib.failure_list(self.db, category="encrypted")
        with sqlite3.connect(self.db) as conn:
            conn.execute(
                "CREATE TRIGGER reject_bulk BEFORE INSERT ON ignored_documents WHEN NEW.document_id=3 BEGIN SELECT RAISE(ABORT,'fixture interrupted'); END"
            )
        with self.assertRaises(sqlite3.IntegrityError):
            lib.set_ignored_matching(self.db, "encrypted", True, chosen["selection_token"], 3)
        self.assertEqual(lib.failure_list(self.db, True)["total"], 0)
        with worker.build_lock(str(self.db)):
            with self.assertRaisesRegex(RuntimeError, "running"):
                lib.set_ignored_matching(self.db, "encrypted", True, chosen["selection_token"], 3)
        package, _ = self.export()
        before = package.read_bytes()
        readonly = lib.failure_list(package, category="encrypted")
        with self.assertRaisesRegex(ValueError, "只读"):
            lib.set_ignored_matching(package, "encrypted", True, readonly["selection_token"], 3)
        self.assertEqual(before, package.read_bytes())

    def managed_retry_database(self):
        folder = self.root / "retry-space"
        folder.mkdir()
        db = folder / "smart-space-knowledge.sqlite"
        with contextlib.closing(sqlite3.connect(self.db)) as source, contextlib.closing(
            sqlite3.connect(db)
        ) as target:
            source.backup(target)
        return folder, db

    def test_selected_retry_rechecks_only_selected_file_and_clears_its_ignore(self):
        other = self.doc.parent / "other.txt"
        other.write_text("其他文件正文必须保留")
        data = json.loads(self.index.read_text())
        data["items"].append(
            dict(path=str(other), name=other.name, root=str(other.parent), isDir=False)
        )
        self.index.write_text(json.dumps(data))
        self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--no-ocr",
        )
        folder, db = self.managed_retry_database()
        with sqlite3.connect(db) as conn:
            conn.execute("UPDATE extraction_state SET status='failed'")
            other_body = conn.execute("SELECT content FROM chunks WHERE document_id=2").fetchall()
        lib.set_ignored(db, [1, 2], True)
        self.doc.write_text("只重试第一个文件的新正文")
        catalog = folder / "catalog"
        catalog.mkdir()
        (catalog / "index.json").write_text("must not replace full catalog")
        args = [
            "--directory",
            folder,
            "--roots-json",
            "[]",
            "--retry-documents-json",
            "[1]",
            "--reserve-gib",
            ".01",
        ]
        result = self.run_script("knowledge_jobs.py", *args)[-1]
        self.assertEqual(result["phase"], "retry-complete")
        self.assertEqual(result["retryFailures"], 0)
        with contextlib.closing(sqlite3.connect(db)) as conn:
            self.assertIn(
                "新正文",
                "".join(
                    r[0] for r in conn.execute("SELECT content FROM chunks WHERE document_id=1")
                ),
            )
            self.assertEqual(
                other_body,
                conn.execute("SELECT content FROM chunks WHERE document_id=2").fetchall(),
            )
            self.assertEqual(lib.ignored_ids(conn), {2})
            self.assertEqual(
                conn.execute(
                    "SELECT status FROM document_availability WHERE document_id=2"
                ).fetchone()[0],
                "present",
            )
        self.assertEqual((catalog / "index.json").read_text(), "must not replace full catalog")

    def test_missing_retry_preserves_old_body_and_records_missing(self):
        folder, db = self.managed_retry_database()
        with sqlite3.connect(db) as conn:
            conn.execute("UPDATE extraction_state SET status='failed'")
        before = lib.fetch(db, 1)["results"]
        self.doc.unlink()
        result = self.run_script(
            "knowledge_jobs.py",
            "--directory",
            folder,
            "--roots-json",
            "[]",
            "--retry-documents-json",
            "[1]",
            "--reserve-gib",
            ".01",
        )[-1]
        self.assertEqual(result["retryFailures"], 1)
        after = lib.fetch(db, 1)
        self.assertEqual(after["extractionStatus"], "missing")
        self.assertEqual(after["results"], before)
        self.assertEqual(after["sourceAvailability"], "unseen")

    def test_ignore_pdf_also_skips_ocr_and_excludes_pending_count(self):
        pdf = self.doc.with_suffix(".pdf")
        pdf.write_bytes(b"invalid fixture must never reach OCR")
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE documents SET path=?,name=?,suffix=?", (str(pdf), pdf.name, "pdf"))
            conn.execute(
                "UPDATE extraction_state SET status='failed',issues_json='[\"page_needs_ocr:1\"]'"
            )
        lib.set_ignored(self.db, [1], True)
        self.index.write_text(
            json.dumps(
                dict(items=[dict(path=str(pdf), name=pdf.name, root=str(pdf.parent), isDir=False)])
            )
        )
        value = self.run_script(
            "smart_space_knowledge.py",
            "build",
            "--db",
            self.db,
            "--index-json",
            self.index,
            "--ocr",
            "--ocr-only",
        )[-1]
        self.assertEqual(value["updated"], 0)
        self.assertEqual(value["reused"], 1)
        self.assertEqual(lib.stats(self.db, True)["ocrPending"]["documents"], 0)

    def test_retry_keeps_exact_nonstandard_database_and_rejects_aikb(self):
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE extraction_state SET status='failed'")
        self.doc.write_text("精确选择自定义数据库的新正文")
        args = [
            "--directory",
            self.root,
            "--retry-db",
            self.db,
            "--roots-json",
            "[]",
            "--retry-documents-json",
            "[1]",
            "--reserve-gib",
            ".01",
        ]
        self.assertEqual(self.run_script("knowledge_jobs.py", *args)[-1]["retryFailures"], 0)
        self.assertFalse((self.root / "smart-space-knowledge.sqlite").exists())
        self.assertIn("新正文", lib.fetch(self.db, 1)["results"][0]["content"])
        package, _ = self.export()
        before = package.read_bytes()
        value = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "knowledge_jobs.py"),
                "--directory",
                str(self.root),
                "--retry-db",
                str(package),
                "--roots-json",
                "[]",
                "--retry-documents-json",
                "[1]",
                "--reserve-gib",
                ".01",
            ],
            text=True,
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(value.returncode, 0)
        self.assertIn("read-only", value.stdout)
        self.assertEqual(package.read_bytes(), before)

    def test_retry_unsupported_format_keeps_body_and_updates_failure_reason(self):
        other = self.doc.with_suffix(".unknown-format")
        self.doc.rename(other)
        with sqlite3.connect(self.db) as conn:
            conn.execute(
                "UPDATE documents SET path=?,name=?,suffix=?",
                (str(other), other.name, "unknown-format"),
            )
            conn.execute(
                "UPDATE extraction_state SET status='failed',issues_json='[\"old reason\"]'"
            )
        before = lib.fetch(self.db, 1)["results"]
        value = self.run_script(
            "knowledge_jobs.py",
            "--directory",
            self.root,
            "--retry-db",
            self.db,
            "--roots-json",
            "[]",
            "--retry-documents-json",
            "[1]",
            "--reserve-gib",
            ".01",
        )[-1]
        self.assertEqual(value["retryFailures"], 1)
        self.assertEqual(before, lib.fetch(self.db, 1)["results"])
        failure = lib.failure_list(self.db)["results"][0]
        self.assertIn("暂不支持", failure["reason"])
        self.assertIn("unsupported", failure["technical_reason"])
        self.assertTrue(failure["attempted_at"])

    def test_historical_failure_descriptions_are_chinese_without_rewriting_library(self):
        cases = [
            ("unsupported container: unknown", "暂不支持此文件"),
            ("unsupported container: ppt-binary", "旧版 PowerPoint（.ppt）"),
            ("Workbook is encrypted", "工作簿已加密"),
            ("unsupported container: ole-unknown", "旧版办公文件"),
            ("pdftotext failed (1)", "PDF 文本提取失败"),
            ("tesseract failed (1)", "图片文字识别失败"),
            ("missing worksheet relationship: ", "工作表关联信息"),
            ("encrypted Word document", "Word 文档已加密"),
            ("Workbook corruption: seen[3] == 4", "工作簿内部结构损坏"),
            ("File is not a zip file", "有效的压缩文档"),
            (
                "'gb18030' codec can't decode byte 0xa9 in position 2038: illegal multibyte sequence",
                "文字编码无法正确解析",
            ),
        ]
        body = lib.fetch(self.db, 1)["results"]
        for raw, expected in cases:
            with self.subTest(raw=raw):
                values = [raw, "previous_chunks_retained"]
                with sqlite3.connect(self.db) as conn:
                    conn.execute(
                        "UPDATE extraction_state SET status='failed',issues_json=?",
                        (json.dumps(values),),
                    )
                before = self.db.read_bytes()
                row = lib.failure_list(self.db)["results"][0]
                self.assertIn(expected, row["reason"])
                self.assertIn("已保留", row["reason"])
                self.assertNotIn("previous_chunks_retained", row["reason"])
                self.assertIn(raw, row["technical_reason"])
                self.assertEqual(lib.stats(self.db, True)["failures"][0]["reason"], row["reason"])
                for script in ("knowledge_library.py", "smart_space_knowledge.py"):
                    output = (
                        self.run_script(script, "--db", self.db, "issues")
                        if script == "knowledge_library.py"
                        else self.run_script(script, "issues", "--db", self.db)
                    )
                    self.assertEqual(output[-1]["results"][0]["issues"], values)
                    self.assertEqual(output[-1]["results"][0]["reason"], row["reason"])
                self.assertEqual(body, lib.fetch(self.db, 1)["results"])
                self.assertEqual(before, self.db.read_bytes())

    def test_chinese_explanations_cover_resources_ocr_and_unknown_errors(self):
        cases = {
            "pdftoppm/tesseract not installed": "缺少",
            "No module named 'olefile'": "读取组件",
            "[Errno 2] No such file or directory: '/tmp/资料.txt'": "找不到来源文件",
            "[Errno 13] Permission denied: '/tmp/资料.txt'": "没有读取文件的权限",
            "Command '['tesseract']' timed out after 120 seconds": "处理超时",
            "database or disk is full": "存储空间不足",
            "MemoryError": "可用内存不足",
            "page_needs_ocr:7": "第 7 页尚需文字识别",
            "empty_ocr_page:3": "第 3 页已完成文字识别",
            "image_needs_ocr": "图片尚需进行文字识别",
            "mismatched tag: line 12": "文档内部结构无法解析",
            "Bad CRC-32 for file": "内部数据校验失败",
            "unexpected future parser failure": "暂未识别出具体原因",
            "系统可用内存不足，OCR 已暂停": "系统可用内存不足，OCR 已暂停",
        }
        for raw, expected in cases.items():
            with self.subTest(raw=raw):
                self.assertIn(expected, lib.describe_issue(raw))
        combined = lib.describe_issue("unsupported container: ppt-binary; previous_chunks_retained")
        self.assertIn("旧版 PowerPoint", combined)
        self.assertIn("已保留", combined)
        self.assertNotIn("unsupported", combined)
        self.assertNotIn("previous_chunks_retained", combined)
        self.assertIn("未记录具体原因", lib.issue_fields([])["reason"])


if __name__ == "__main__":
    unittest.main()
