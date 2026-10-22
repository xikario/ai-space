"""Explicit, age-filtered removal of retained absent knowledge, never originals."""
import contextlib
from datetime import datetime, timedelta, timezone
import fcntl
import hashlib
import json
from pathlib import Path
import sqlite3
import time

from knowledge_library import (
    emit,
    manifest,
    open_readonly,
    guard_storage,
    registry,
    StorageLimitError,
)


def retention_cutoff(days):
    if not 1 <= days <= 3650:
        raise ValueError("保留天数必须在 1–3650 天之间")
    return (datetime.now(timezone.utc) - timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ")


def scope(db, path, cutoff, days):
    for row in registry()["libraries"]:
        try:
            same = Path(row["path"]).expanduser().resolve() == path
        except (OSError, ValueError, RuntimeError):
            continue
        if same and row.get("read_only"):
            raise ValueError("挂载的历史库为只读，不能清理；请在来源个人库维护或导出新版本")
    identity = manifest(db, path)
    if identity.get("format") != "legacy-sqlite":
        raise ValueError("导入的 AIKB 为只读库，请在来源电脑上维护")
    columns = {row[1] for row in db.execute("PRAGMA table_info(document_availability)")}
    meta = dict(db.execute("SELECT key,value FROM meta"))
    if "absent_since" not in columns or meta.get("fts_rowid_layout") != "chunk-id-v1":
        raise ValueError("请先显式执行一次文本建库以升级保留时间和索引；读取旧库不会自动升级")
    count = db.execute(
        "SELECT count(*) FROM document_availability WHERE status='unseen' AND absent_since!='' AND absent_since<=?",
        (cutoff,),
    ).fetchone()[0]
    token = hashlib.sha256(
        json.dumps(
            [
                identity["library_id"],
                meta.get("failure_epoch"),
                meta.get("failure_revision"),
                cutoff,
                days,
                count,
            ],
            ensure_ascii=False,
        ).encode()
    ).hexdigest()
    examples = [
        dict(document_id=row[0], name=row[1], absentSince=row[2])
        for row in db.execute(
            "SELECT d.id,d.name,a.absent_since FROM document_availability a JOIN documents d ON d.id=a.document_id WHERE a.status='unseen' AND a.absent_since!='' AND a.absent_since<=? ORDER BY a.document_id LIMIT 10",
            (cutoff,),
        )
    ]
    return dict(
        done=True,
        task="prune-absent",
        count=count,
        cutoff=cutoff,
        days=days,
        selection_token=token,
        examples=examples,
        files_deleted=False,
    )


def prune_absent(
    path, days=30, cutoff="", selection_token="", apply=False, reserve_gib=2, max_gib=32
):
    days = int(days)
    latest = retention_cutoff(days)
    cutoff = cutoff or latest
    datetime.strptime(cutoff, "%Y-%m-%dT%H:%M:%SZ")
    if cutoff > latest:
        raise ValueError("清理范围不能短于指定保留天数，请重新预览")
    path = Path(path).expanduser().resolve()
    if not apply:
        with contextlib.closing(open_readonly(path)) as db:
            db.execute("BEGIN")
            return scope(db, path, cutoff, days)
    if not selection_token:
        raise ValueError("请先预览并确认清理范围")
    from smart_space_knowledge import build_lock, delete_fts_document, mark_availability

    # The pipeline owns this lock before its manifest stage acquires build_lock.
    with (path.parent / ".pipeline.lock").open("a") as pipeline:
        try:
            fcntl.flock(pipeline, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("建库正在运行，请暂停或等待完成后再清理")
        with build_lock(path), contextlib.closing(
            sqlite3.connect(path.as_uri() + "?mode=rw", uri=True, timeout=5)
        ) as db:
            db.execute("PRAGMA foreign_keys=ON")
            db.execute("BEGIN IMMEDIATE")
            current = scope(db, path, cutoff, days)
            if current["selection_token"] != selection_token:
                raise ValueError("知识库或保留状态已变化，请重新预览后确认")

            def check_storage():
                try:
                    guard_storage(path, reserve_gib, max_gib)
                except StorageLimitError as error:
                    raise RuntimeError(
                        "清理需要临时事务日志，但已达到单库容量或磁盘保留上限；本次未提交的清理会回滚，请检查空间及容量设置后重试"
                    ) from error

            deleted = restored = last = 0
            next_progress = 0
            try:
                while True:
                    rows = db.execute(
                        "SELECT d.id,d.path FROM document_availability a JOIN documents d ON d.id=a.document_id WHERE a.status='unseen' AND a.absent_since!='' AND a.absent_since<=? AND a.document_id>? ORDER BY a.document_id LIMIT 200",
                        (cutoff, last),
                    ).fetchall()
                    if not rows:
                        break
                    check_storage()
                    for ident, original in rows:
                        last = ident
                        if Path(original).is_file():
                            mark_availability(db, ident, True)
                            restored += 1
                        else:
                            delete_fts_document(db, ident)
                            db.execute("DELETE FROM document_names_fts WHERE rowid=?", (ident,))
                            db.execute("DELETE FROM documents WHERE id=?", (ident,))
                            deleted += 1
                        if time.monotonic() >= next_progress:
                            emit(
                                task="prune-absent",
                                phase="prune",
                                processed=deleted + restored,
                                total=current["count"],
                            )
                            next_progress = time.monotonic() + 2
                check_storage()
                db.commit()
            except BaseException:
                db.rollback()
                raise
            return dict(
                done=True,
                task="prune-absent",
                deleted=deleted,
                restored=restored,
                files_deleted=False,
                message="只清理库内正文，原文件未删除；空闲页可复用，库文件不一定立即缩小",
            )
