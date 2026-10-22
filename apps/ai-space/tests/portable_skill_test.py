"""A build-exported Skill searches AIKB with no app, source files or originals."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

skill = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import knowledge_library as library
import smart_space_knowledge as worker

with tempfile.TemporaryDirectory(prefix="portable-skill-") as temporary:
    root = Path(temporary)
    database = root / "source.sqlite"
    connection = worker.open_db(str(database))
    text = "离线知识示例：没有原文件也能检索正文和出处。"
    continuation = "补充段落：跨页读取保留同一份文档引用。"
    connection.execute(
        "INSERT INTO documents(id,path,name,content_hash) VALUES(1,?,?,?)",
        (
            "/missing/example.txt",
            "example.txt",
            hashlib.sha256((text + continuation).encode()).hexdigest(),
        ),
    )
    connection.execute(
        "INSERT INTO chunks(id,document_id,ordinal,locator,content) VALUES(1,1,0,'段落 1',?)", (text,)
    )
    connection.execute(
        "INSERT INTO chunks_fts(rowid,name,path,content,locator,document_id,chunk_id) VALUES(1,'example','',?,'段落 1',1,1)",
        (worker.fts_text(text),),
    )
    connection.execute(
        "INSERT INTO chunks(id,document_id,ordinal,locator,content) VALUES(2,1,1,'段落 2',?)",
        (continuation,),
    )
    connection.execute(
        "INSERT INTO chunks_fts(rowid,name,path,content,locator,document_id,chunk_id) VALUES(2,'example','',?,'段落 2',1,2)",
        (worker.fts_text(continuation),),
    )
    connection.execute("INSERT INTO extraction_state(document_id,status) VALUES(1,'complete')")
    connection.commit()
    connection.close()
    package = root / "shared.aikb"
    with contextlib.redirect_stdout(io.StringIO()):
        library.export_library(database, package, "示例知识", 0.01, 1)
    exported = root / "agent-skill"
    shutil.copytree(skill, exported)
    database.unlink()
    environment = dict(
        os.environ,
        HOME=str(root / "home"),
        XDG_CONFIG_HOME=str(root / "config"),
        XDG_DATA_HOME=str(root / "data"),
        PATH=str(root / "empty-bin"),
        PYTHONPATH="",
    )
    package_before = package.read_bytes()
    result = subprocess.run(
        [
            sys.executable,
            "-I",
            str(exported / "scripts/query.py"),
            "--db",
            str(package),
            "search",
            "--query",
            "离线知识示例",
        ],
        env=environment,
        cwd=root,
        capture_output=True,
        text=True,
        timeout=15,
        check=True,
    )
    answer = json.loads(result.stdout)
    assert answer["results"], answer
    assert "离线知识示例" in answer["results"][0]["snippet"]
    assert answer["results"][0]["reference"].startswith("aispace://")
    reference = answer["results"][0]["reference"]

    def query(*arguments):
        completed = subprocess.run(
            [
                sys.executable,
                "-I",
                str(exported / "scripts/query.py"),
                "--db",
                str(package),
                *arguments,
            ],
            env=environment,
            cwd=root,
            capture_output=True,
            text=True,
            timeout=15,
            check=True,
        )
        return json.loads(completed.stdout)

    status = query("stats")
    assert status["documents"] == 1 and status["chunks"] == 2, status
    first = query("fetch", "--reference", reference, "--limit", "1")
    assert first["results"][0]["content"] == text, first
    assert first["content_trust"] == "untrusted_document", first
    assert first["hasMore"] and first["nextOffset"] == 1, first
    second = query(
        "fetch", "--reference", reference, "--offset", str(first["nextOffset"]), "--limit", "1"
    )
    assert second["results"][0]["content"] == continuation and not second["hasMore"], second
    legacy = query("--status")
    assert legacy["documents"] == 1, legacy
    assert package.read_bytes() == package_before, "Read-only Skill modified the library"
    assert not database.exists()
print(
    "PASS: portable Skill stats, search, paged fetch and legacy calls use only AIKB without modifying it"
)
