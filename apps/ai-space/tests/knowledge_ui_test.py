"""Native GUI consumes an imported knowledge library with no source snapshot."""
import ast
import json
import os
from pathlib import Path
import re
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


def call(method, *args):
    output = subprocess.check_output(
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
            *map(str, args),
        ],
        text=True,
        stderr=subprocess.DEVNULL,
    )
    return json.loads(ast.literal_eval(output)[0]) if method == "status" else output


def wait(predicate, label):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            value = call("status")
        except subprocess.CalledProcessError:
            time.sleep(0.05)
            continue
        if predicate(value):
            check(True, label)
            return value
        time.sleep(0.05)
    if os.environ.get("AI_SPACE_TEST_ARTIFACT_DIR") and value.get("windowId"):
        subprocess.run(
            [
                "import",
                "-window",
                value["windowId"],
                str(Path(os.environ["AI_SPACE_TEST_ARTIFACT_DIR"]) / "failed-ui-state.png"),
            ],
            check=True,
        )
    raise AssertionError(label + ": " + json.dumps(value, ensure_ascii=False))


with tempfile.TemporaryDirectory(prefix="aikb-ui-") as folder:
    root = Path(folder)
    home = root / "other-user"
    home.mkdir()
    env = dict(
        os.environ,
        HOME=str(home),
        XDG_CONFIG_HOME=str(home / ".config"),
        XDG_DATA_HOME=str(home / ".local/share"),
        XDG_CACHE_HOME=str(home / ".cache"),
        AI_SPACE_TEST_NO_BOOTSTRAP="1",
        AI_SPACE_EXPLICIT_SHOW="1",
        GIO_USE_VFS="local",
    )
    source = root / "publisher-files"
    source.mkdir()
    original = source / "离线银行方案.txt"
    original.write_text("宁波银行 KMS 离线知识包的正文：统一密钥管理部署。")
    index = root / "index.json"
    index.write_text(
        json.dumps(
            dict(
                items=[
                    dict(
                        path=str(original),
                        root=str(source),
                        name=original.name,
                        category="document",
                        isDir=False,
                    )
                ]
            )
        )
    )
    db = root / "source.sqlite"
    package = root / "shared.aikb"

    def run(script, *args):
        subprocess.run(
            [sys.executable, str(scripts / script), *map(str, args)],
            env=env,
            check=True,
            stdout=subprocess.DEVNULL,
        )

    run("smart_space_knowledge.py", "build", "--db", db, "--index-json", index, "--no-ocr")
    run("knowledge_library.py", "--db", db, "export", "--output", package, "--name", "离线示范库")
    run(
        "knowledge_library.py",
        "import",
        "--source",
        package,
        "--directory",
        root / "mounted",
        "--mode",
        "mount",
    )
    original.unlink()
    index.unlink()
    db.unlink()
    build_sources = root / "资料 目录"
    build_sources.mkdir()
    (build_sources / "工作材料.pdf").write_bytes(b"%PDF-1.4 fixture")
    managed = root / "知识库 空间"
    settings = home / ".config/xikario"
    settings.mkdir(parents=True, exist_ok=True)

    def ini_path(path):
        return '"' + "".join(("\\x%04x" % ord(c)) if ord(c) > 127 else c for c in str(path)) + '"'

    (settings / "ai-space.ini").write_text(
        "[smartSpace]\nstorageConfirmed=true\nlibraryStorageDirectory="
        + ini_path(managed)
        + "\nroots="
        + ini_path(build_sources)
        + "\n"
    )
    tools = root / "tools"
    tools.mkdir()
    converter = tools / "pdftotext"
    converter.write_text(
        "#!"
        + sys.executable
        + '\nimport time,sys\nfrom pathlib import Path\ntime.sleep(6)\nPath(sys.argv[-1]).write_text("构建文档正文")\n'
    )
    converter.chmod(0o755)
    env["PATH"] = str(tools) + os.pathsep + env["PATH"]
    check(not list(home.rglob("index.json")), "receiving user has no file snapshot")
    with (root / "app.log").open("w") as log:
        process = subprocess.Popen(
            [str(binary), "--show"], env=env, stdout=log, stderr=subprocess.STDOUT
        )
    try:
        state = wait(
            lambda s: s.get("ready") and s.get("visible"), "fresh receiving-user app is ready"
        )
        check(not state["indexBusy"], "startup does not build or scan")
        call("search", "宁波银行 KMS")
        state = wait(
            lambda s: s.get("results") == 1,
            "imported library appears without original files or local index entries",
        )
        info = subprocess.check_output(["xwininfo", "-id", state["windowId"]], text=True)
        width = int(re.search(r"Width: (\d+)", info).group(1))
        ratio = width / state["width"]
        subprocess.run(
            [
                "xdotool",
                "mousemove",
                "--sync",
                "--window",
                state["windowId"],
                str(round((state["width"] - 65) * ratio)),
                str(round(160 * ratio)),
            ],
            check=True,
        )
        time.sleep(0.15)
        subprocess.run(["xdotool", "click", "1"], check=True)
        time.sleep(0.4)
        if os.environ.get("AI_SPACE_TEST_ARTIFACT_DIR"):
            target = Path(os.environ["AI_SPACE_TEST_ARTIFACT_DIR"])
            target.mkdir(parents=True, exist_ok=True)
            subprocess.run(
                ["import", "-window", state["windowId"], str(target / "offline-preview-debug.png")],
                check=True,
            )
        state = wait(
            lambda s: s.get("previewVisible") and s.get("libraryPreviewLoaded"),
            "clicking library result fetches and renders stored body",
        )
        screenshot = os.environ.get("AI_SPACE_TEST_ARTIFACT_DIR")
        if screenshot:
            target = Path(screenshot)
            target.mkdir(parents=True, exist_ok=True)
            subprocess.run(
                [
                    "import",
                    "-window",
                    state["windowId"],
                    str(target / "offline-library-preview.png"),
                ],
                check=True,
            )
        subprocess.run(["xdotool", "key", "Escape"], check=True)
        call("showKnowledgeStatus")
        wait(
            lambda s: s.get("knowledgeStatusVisible"),
            "status details open even without a personal database",
        )
        if screenshot:
            subprocess.run(
                ["import", "-window", "root", str(Path(screenshot) / "knowledge-status.png")],
                check=True,
            )
        subprocess.run(["xdotool", "key", "Escape"], check=True)
        wait(
            lambda s: not s.get("knowledgeStatusVisible"),
            "closing status details leaves the app usable",
        )
        call("showKnowledgeManager")
        wait(
            lambda s: s.get("knowledgeManagerVisible"),
            "storage and staged-build manager opens from application API",
        )
        call("showKnowledgeManager")
        call("startFullIndex")
        time.sleep(0.15)
        check(not call("status")["indexBusy"], "no build starts while manager is open")
        windows = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^知识库管理与建库向导$"], text=True
        ).splitlines()
        check(len(windows) == 1, "repeated D-Bus manager requests do not stack dialogs")
        if screenshot:
            subprocess.run(
                ["import", "-window", "root", str(Path(screenshot) / "knowledge-manager.png")],
                check=True,
            )
        manager = subprocess.check_output(
            ["xdotool", "search", "--onlyvisible", "--name", "^知识库管理与建库向导$"], text=True
        ).splitlines()[-1]
        geometry = subprocess.check_output(["xwininfo", "-id", manager], text=True)
        mw = int(re.search(r"Width: (\d+)", geometry).group(1))
        mh = int(re.search(r"Height: (\d+)", geometry).group(1))
        scale = mw / 780
        for step in (1, 2, 3):
            subprocess.run(
                [
                    "xdotool",
                    "mousemove",
                    "--sync",
                    "--window",
                    manager,
                    str(round(mw - 65 * scale)),
                    str(round(mh - 93 * scale)),
                    "click",
                    "1",
                ],
                check=True,
            )
            wait(
                lambda s: s.get("knowledgeWizardStep") == step,
                "wizard advances in order to step " + str(step),
            )
        if screenshot:
            subprocess.run(
                ["import", "-window", "root", str(Path(screenshot) / "knowledge-wizard-ocr.png")],
                check=True,
            )

        subprocess.run(["xdotool", "key", "Escape"], check=True)
        wait(
            lambda s: not s.get("knowledgeManagerVisible"),
            "canceling manager leaves application usable",
        )
        call("search", "不存在的客户")
        wait(lambda s: s.get("results") == 0, "new query removes stale imported results")
        call("search", "宁波银行 KMS")
        call("search", "不存在的客户")
        wait(
            lambda s: s.get("results") == 0,
            "superseded asynchronous query cannot repopulate new results",
        )
        time.sleep(1)
        check(
            call("status")["results"] == 0,
            "late process completion does not restore stale evidence",
        )
        check(
            not list(home.rglob("smart-space-knowledge.sqlite")),
            "import-only usage creates no second personal database",
        )
        if "--build-reader" in sys.argv:
            call("startFullIndex")
            wait(
                lambda s: s.get("indexBusy"),
                "explicit staged build starts on the selected storage disk",
            )
            call("search", "不存在的客户")
            wait(lambda s: s.get("results") == 0, "old results clear while writer is active")
            call("search", "宁波银行 KMS")
            current = wait(
                lambda s: s.get("results") == 1,
                "knowledge retrieval remains usable during a text writer",
            )
            check(
                current["indexBusy"], "search finishes while the background build is still running"
            )
            subprocess.run(
                [
                    "xdotool",
                    "mousemove",
                    "--sync",
                    "--window",
                    current["windowId"],
                    str(round((current["width"] - 18) * ratio)),
                    str(round(17 * ratio)),
                    "click",
                    "1",
                ],
                check=True,
            )
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                prompt = subprocess.run(
                    ["xdotool", "search", "--onlyvisible", "--name", "^建库任务正在运行"],
                    text=True,
                    stdout=subprocess.PIPE,
                )
                if prompt.returncode == 0:
                    break
                time.sleep(0.05)
            check(prompt.returncode == 0, "closing a busy main window asks how to handle the task")
            subprocess.run(["xdotool", "key", "Escape"], check=True)

            call("showKnowledgeStatus")
            wait(
                lambda s: s.get("knowledgeStatusVisible"),
                "status details available while a writer is running",
            )
            subprocess.run(["xdotool", "key", "Escape"], check=True)
            wait(
                lambda s: not s.get("knowledgeStatusVisible"),
                "closing details does not stop the writer",
            )
            wait(
                lambda s: not s.get("indexBusy") and not s.get("indexLoading"),
                "staged job completes and publishes its new file snapshot",
            )
            check(
                (managed / "smart-space-knowledge.sqlite").exists(),
                "new personal database uses the chosen disk",
            )
            call("showKnowledgeStatus")
            wait(
                lambda s: s.get("knowledgeStatusLoaded"),
                "details read actual committed library statistics",
            )
            if screenshot:
                subprocess.run(
                    [
                        "import",
                        "-window",
                        "root",
                        str(Path(screenshot) / "knowledge-status-built.png"),
                    ],
                    check=True,
                )
            subprocess.run(["xdotool", "key", "Escape"], check=True)
            wait(lambda s: not s.get("knowledgeStatusVisible"), "close statistics popup")
            check(
                not list(home.rglob("smart-space-knowledge.sqlite")),
                "staged build does not create a default-home database",
            )
            (build_sources / "工作材料.pdf").write_bytes(b"%PDF-1.4 changed fixture")
            call("startFullIndex")
            wait(lambda s: s.get("indexBusy"), "second job starts for pause regression")
            call("pauseKnowledgeTask")
            wait(lambda s: not s.get("indexBusy"), "pause controller returns to idle")
            (build_sources / "工作材料.pdf").write_bytes(b"%PDF-1.4 quit fixture")
            call("startFullIndex")
            wait(
                lambda s: s.get("indexBusy"),
                "active worker starts before graceful application quit",
            )

        call("quit")
        check(process.wait(timeout=10) == 0, "application and library workers exit normally")
        with (root / "autostart.log").open("w") as log:
            process = subprocess.Popen(
                [str(binary), "--autostart"], env=env, stdout=log, stderr=subprocess.STDOUT
            )
        current = wait(
            lambda s: s.get("ready") and s.get("minimized") and not s.get("visible"),
            "autostart restores app to taskbar without opening main window",
        )
        subprocess.run([str(binary), "--autostart"], env=env, check=True, timeout=5)
        unchanged = call("status")
        check(
            unchanged["pid"] == current["pid"]
            and unchanged["minimized"]
            and not unchanged["visible"],
            "duplicate autostart preserves minimized existing instance",
        )
        call("quit")
        check(process.wait(timeout=10) == 0, "autostart app closes cleanly")

        if "--build-reader" in sys.argv:
            result = subprocess.check_output(
                [sys.executable, str(scripts / "knowledge_library.py"), "stats"], env=env, text=True
            )
            check(
                json.loads(result)["documents"] == 1,
                "standalone reader discovers the actual Chinese path written by QSettings",
            )
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
