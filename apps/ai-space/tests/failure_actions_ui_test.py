"""Native failure-list ignore, restore and selected retry with a disposable user."""
import ast
import contextlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
scripts = Path(__file__).resolve().parents[1] / "scripts"


def check(ok, label):
    if not ok:
        raise AssertionError(label)
    print("PASS:", label, flush=True)


def call(method):
    value = subprocess.check_output(
        [
            "gdbus",
            "call",
            "--session",
            "--dest",
            "io.github.xikario.AiSpace",
            "--object-path",
            "/aiSpace",
            "--method",
            "io.github.xikario.AiSpace." + method,
        ],
        text=True,
        stderr=subprocess.DEVNULL,
    )
    return json.loads(ast.literal_eval(value)[0]) if method == "status" else value


def wait(predicate, label):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            if predicate():
                check(True, label)
                return
        except (subprocess.CalledProcessError, sqlite3.Error):
            pass
        time.sleep(0.1)
    artifact = os.environ.get("AI_SPACE_TEST_ARTIFACT_DIR")
    if artifact and "window" in globals():
        target = Path(artifact)
        target.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["import", "-window", window, str(target / "failure-timeout.png")], check=False
        )
    try:
        print("LAST STATUS:", json.dumps(call("status"), ensure_ascii=False), flush=True)
    except subprocess.CalledProcessError:
        pass
    raise AssertionError(label)


def key(value):
    subprocess.run(["xdotool", "key", "--clearmodifiers", value], check=True)


def category(position, expected):
    key("alt+c")
    key("alt+Down")
    key("Home")
    for _ in range(position):
        key("Down")
    key("Return")
    wait(
        lambda: call("status").get("failureListCategory") == expected
        and call("status").get("failureListLoaded"),
        "reason category " + expected + " is loaded",
    )


with tempfile.TemporaryDirectory(prefix="failure-ui-") as temporary:
    root = Path(temporary)
    home = root / "user"
    home.mkdir()
    source = root / "sources"
    source.mkdir()
    doc = source / "retry.txt"
    doc.write_text("原来的正文，忽略时必须保留。")
    folder = root / "knowledge"
    folder.mkdir()
    db = folder / "smart-space-knowledge.sqlite"
    index = root / "index.json"
    index.write_text(
        json.dumps(dict(items=[dict(path=str(doc), root=str(source), name=doc.name, isDir=False)]))
    )
    env = dict(
        os.environ,
        HOME=str(home),
        XDG_CONFIG_HOME=str(home / ".config"),
        XDG_CACHE_HOME=str(home / ".cache"),
        XDG_DATA_HOME=str(home / ".local/share"),
        AI_SPACE_TEST_NO_BOOTSTRAP="1",
        AI_SPACE_EXPLICIT_SHOW="1",
        GIO_USE_VFS="local",
    )
    subprocess.run(
        [
            sys.executable,
            str(scripts / "smart_space_knowledge.py"),
            "build",
            "--db",
            str(db),
            "--index-json",
            str(index),
            "--no-ocr",
        ],
        env=env,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    with contextlib.closing(sqlite3.connect(db)) as conn:
        conn.execute(
            "UPDATE extraction_state SET status='failed',issues_json=?",
            (json.dumps(["unsupported container: ppt-binary", "previous_chunks_retained"]),),
        )
        conn.commit()

    def state():
        with contextlib.closing(sqlite3.connect("file:" + str(db) + "?mode=ro", uri=True)) as conn:
            body = "".join(r[0] for r in conn.execute("SELECT content FROM chunks"))
            policy = conn.execute(
                "SELECT 1 FROM sqlite_master WHERE name='ignored_documents'"
            ).fetchone()
            ignored = (
                conn.execute("SELECT count(*) FROM ignored_documents").fetchone()[0]
                if policy
                else 0
            )
            status = conn.execute(
                "SELECT status FROM extraction_state WHERE document_id=1"
            ).fetchone()[0]
            return ignored, status, body

    before = state()[2]
    settings = home / ".config/xikario"
    settings.mkdir(parents=True)

    def settled(ignored):
        return state()[0] == ignored and call("status").get("failureListLoaded")

    (settings / "ai-space.ini").write_text(
        "[smartSpace]\nknowledgeDirectory="
        + str(folder)
        + "\nlibraryStorageDirectory="
        + str(folder)
        + "\nstorageConfirmed=true\nroots="
        + str(source)
        + "\nthemeMode="
        + os.environ.get("AI_SPACE_TEST_THEME_MODE", "1")
        + "\n"
    )
    with (root / "app.log").open("w") as log:
        process = subprocess.Popen(
            [str(binary), "--show"], env=env, stdout=log, stderr=subprocess.STDOUT
        )
    try:
        wait(lambda: call("status").get("ready"), "test application is ready")
        call("showKnowledgeStatus")
        wait(
            lambda: call("status").get("knowledgeStatusLoaded"),
            "status popup reads failure statistics",
        )
        key("alt+f")
        wait(
            lambda: call("status").get("knowledgeFailuresVisible")
            and call("status").get("failureListLoaded"),
            "failure manager opens from status popup",
        )
        window = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^知识库 · 失败项与忽略$"], text=True
        ).splitlines()[-1]
        subprocess.run(["xdotool", "windowfocus", "--sync", window], check=True)
        key("ctrl+a")
        artifact = os.environ.get("AI_SPACE_TEST_ARTIFACT_DIR")
        if artifact:
            target = Path(artifact)
            target.mkdir(parents=True, exist_ok=True)
            subprocess.run(
                ["import", "-window", window, str(target / "failure-manager.png")], check=True
            )
        key("alt+i")
        wait(lambda: settled(1), "ignoring selected failure is saved")
        check(
            state()[1] == "failed" and state()[2] == before,
            "ignore preserves real quality state and existing body",
        )
        key("alt+2")
        wait(
            lambda: call("status").get("failureListIgnored")
            and call("status").get("failureListLoaded"),
            "ignored records can be revisited",
        )
        key("ctrl+a")
        key("alt+u")
        wait(lambda: settled(0), "cancel ignore restores pending handling")
        key("alt+1")
        wait(
            lambda: not call("status").get("failureListIgnored")
            and call("status").get("failureListLoaded"),
            "pending list is available again",
        )
        key("ctrl+a")
        key("alt+i")
        wait(lambda: settled(1), "failure can be ignored again before explicit retry")
        key("alt+2")
        wait(
            lambda: call("status").get("failureListIgnored")
            and call("status").get("failureListLoaded"),
            "ignored file remains selectable",
        )
        doc.write_text("重试已重新读取当前原始文件的新正文。")
        key("ctrl+a")
        key("alt+o")
        key("alt+r")
        wait(
            lambda: state()[0] == 0 and state()[1] == "complete" and "新正文" in state()[2],
            "selected retry clears ignore and extracts current file",
        )
        wait(
            lambda: not call("status").get("indexBusy")
            and not call("status").get("knowledgeStatusVisible"),
            "retry finishes with dialogs closed and app usable",
        )
        check(
            not (folder / "catalog/index.json").exists(),
            "retry does not replace or create a full scan catalog",
        )

        with contextlib.closing(sqlite3.connect(db)) as conn:
            for number in range(62):
                ident = conn.execute(
                    "INSERT INTO documents(path,name) VALUES(?,?)",
                    (str(source / ("category-%d.xls" % number)), "category-%d.xls" % number),
                ).lastrowid
                reason = (
                    "Workbook is encrypted" if number < 61 else "unsupported container: unknown"
                )
                conn.execute(
                    "INSERT INTO extraction_state(document_id,status,issues_json) VALUES(?,'failed',?)",
                    (ident, json.dumps([reason, "previous_chunks_retained"])),
                )
            conn.commit()
        call("showKnowledgeStatus")
        wait(
            lambda: call("status").get("knowledgeStatusLoaded"),
            "status popup refreshes new categorized failures",
        )
        key("alt+f")
        wait(
            lambda: call("status").get("knowledgeFailuresVisible")
            and call("status").get("failureListLoaded"),
            "failure manager reopens for categorized batches",
        )
        window = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^知识库 · 失败项与忽略$"], text=True
        ).splitlines()[-1]
        subprocess.run(["xdotool", "windowfocus", "--sync", window], check=True)
        category(1, "encrypted")
        check(call("status").get("failureListTotal") == 61, "encrypted category covers both pages")
        key("alt+a")
        wait(
            lambda: call("status").get("failureSelectedCount") == 61
            and call("status").get("failureAllSelected"),
            "select all includes files on other pages",
        )
        key("alt+Right")
        wait(
            lambda: call("status").get("failureListLoaded")
            and call("status").get("failureListOffset") == 50,
            "second filtered page is loaded",
        )
        check(
            call("status").get("failureSelectedCount") == 61
            and call("status").get("failureAllSelected"),
            "cross-page selection survives pagination",
        )
        key("ctrl+a")
        wait(
            lambda: call("status").get("failureSelectedCount") == 11
            and not call("status").get("failureAllSelected"),
            "Ctrl+A switches to selecting only the visible page",
        )
        key("alt+a")
        wait(
            lambda: call("status").get("failureSelectedCount") == 61
            and call("status").get("failureAllSelected"),
            "cross-page selection can be enabled again",
        )
        category(2, "unsupported")
        check(
            call("status").get("failureSelectedCount") == 0
            and not call("status").get("failureAllSelected"),
            "changing category clears previous scope selection",
        )
        category(1, "encrypted")
        key("alt+a")
        wait(
            lambda: call("status").get("failureSelectedCount") == 61,
            "encrypted category can be selected again",
        )
        if artifact:
            subprocess.run(
                [
                    "import",
                    "-window",
                    window,
                    str(Path(artifact) / "failure-category-selection.png"),
                ],
                check=True,
            )
        key("alt+i")
        wait(lambda: settled(61), "all 61 encrypted files are ignored in one action")
        check(
            call("status").get("failureListTotal") == 0,
            "ignored encrypted files leave the pending category",
        )
        with contextlib.closing(sqlite3.connect(db)) as conn:
            check(
                conn.execute(
                    "SELECT count(*) FROM extraction_state WHERE status='failed'"
                ).fetchone()[0]
                == 62,
                "batch ignore keeps real failure states",
            )
            check(
                conn.execute(
                    "SELECT count(*) FROM ignored_documents WHERE document_id=(SELECT max(id) FROM documents)"
                ).fetchone()[0]
                == 0,
                "unrelated unsupported file is not ignored",
            )
        key("alt+2")
        wait(
            lambda: call("status").get("failureListIgnored")
            and call("status").get("failureListLoaded"),
            "ignored view retains reason category",
        )
        check(
            call("status").get("failureListTotal") == 61,
            "all ignored encrypted records can be found again",
        )
        key("alt+a")
        wait(
            lambda: call("status").get("failureSelectedCount") == 61
            and call("status").get("failureAllSelected"),
            "all ignored encrypted files are selected across pages",
        )
        key("alt+u")
        wait(lambda: settled(0), "cross-page cancel ignore restores all encrypted files")
        check("新正文" in state()[2], "batch ignore and restore preserve indexed body")
        key("Escape")
        key("Escape")
        with contextlib.closing(sqlite3.connect(db)) as conn:
            missing = source / "已移除的旧资料.txt"
            ident = conn.execute(
                "INSERT INTO documents(path,name) VALUES(?,?)", (str(missing), missing.name)
            ).lastrowid
            chunk = conn.execute(
                "INSERT INTO chunks(document_id,ordinal,content) VALUES(?,0,'可清理的旧正文')", (ident,)
            ).lastrowid
            conn.execute(
                "INSERT INTO chunks_fts(rowid,document_id,chunk_id,content) VALUES(?,?,?,'可清理的旧正文')",
                (chunk, ident, chunk),
            )
            conn.execute(
                "INSERT INTO extraction_state(document_id,status) VALUES(?,'complete')", (ident,)
            )
            conn.execute(
                "INSERT INTO document_availability(document_id,status,absent_since) VALUES(?,'unseen','2000-01-01T00:00:00Z')",
                (ident,),
            )
            conn.commit()

        def retained():
            with contextlib.closing(sqlite3.connect(db)) as conn:
                return conn.execute(
                    "SELECT count(*) FROM documents WHERE id=?", (ident,)
                ).fetchone()[0]

        call("showKnowledgeStatus")
        wait(
            lambda: call("status").get("knowledgeStatusVisible")
            and call("status").get("knowledgeStatusLoaded")
            and call("status").get("knowledgePruneEnabled"),
            "status opens with retained absent document",
        )
        status_window = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^知识库 · 当前状态详情$"], text=True
        ).splitlines()[-1]
        subprocess.run(["xdotool", "windowfocus", "--sync", status_window], check=True)
        key("alt+d")
        wait(
            lambda: call("status").get("knowledgePruneVisible"), "age-filtered cleanup dialog opens"
        )
        check(retained() == 1, "opening cleanup does not delete knowledge")
        prune_window = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^清理保留的缺失正文"], text=True
        ).splitlines()[-1]
        subprocess.run(["xdotool", "windowfocus", "--sync", prune_window], check=True)
        key("alt+p")
        wait(
            lambda: call("status").get("knowledgePruneReady"),
            "cleanup preview enables explicit confirmation",
        )
        check(retained() == 1, "preview keeps knowledge unchanged")
        key("alt+r")
        time.sleep(0.15)
        confirm_window = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^确认清理库内正文$"], text=True
        ).splitlines()[-1]
        subprocess.run(["xdotool", "windowfocus", "--sync", confirm_window], check=True)
        key("Return")
        check(retained() == 1, "confirmation defaults to No")
        subprocess.run(["xdotool", "windowfocus", "--sync", prune_window], check=True)
        key("alt+r")
        time.sleep(0.15)
        confirm_window = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^确认清理库内正文$"], text=True
        ).splitlines()[-1]
        subprocess.run(["xdotool", "windowfocus", "--sync", confirm_window], check=True)
        key("Left")
        key("Return")
        wait(lambda: retained() == 0, "confirmed cleanup removes only selected old knowledge")
        check("新正文" in state()[2], "cleanup preserves other source documents")
        subprocess.run(["xdotool", "windowfocus", "--sync", prune_window], check=True)
        key("Escape")
        wait(lambda: not call("status").get("knowledgePruneVisible"), "cleanup window closes")
        key("Escape")
        call("quit")
        check(process.wait(timeout=10) == 0, "application exits without remaining failure workers")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
