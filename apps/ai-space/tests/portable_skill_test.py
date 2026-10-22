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
    connection.execute(
        "INSERT INTO documents(id,path,name,content_hash) VALUES(1,?,?,?)",
        ("/missing/example.txt", "example.txt", hashlib.sha256(text.encode()).hexdigest()),
    )
    connection.execute(
        "INSERT INTO chunks(id,document_id,ordinal,locator,content) VALUES(1,1,0,'段落 1',?)", (text,)
    )
    connection.execute(
        "INSERT INTO chunks_fts(rowid,name,path,content,locator,document_id,chunk_id) VALUES(1,'example','',?,'段落 1',1,1)",
        (worker.fts_text(text),),
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
    assert not database.exists()
print("PASS: portable Skill uses only the exported AIKB and its adjacent query core")
