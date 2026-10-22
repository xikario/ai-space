#!/usr/bin/env python3
"""AIKB v1: indexed, single-file knowledge snapshots and a Qt-free query API."""
import argparse
import ctypes
import errno
import contextlib
import configparser

try:
    import fcntl
except ImportError:
    fcntl = None
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import sqlite3
import sys
import tempfile
import time
import uuid

APPLICATION_ID = 0x41494B42
FORMAT_VERSION = 1
GIB = 1024 ** 3


class StorageLimitError(OSError):
    pass


TOKEN = re.compile(r"[0-9A-Za-z_]+|[\u3400-\u9fff]")
SEGMENT = re.compile(r"[0-9A-Za-z_.-]+|[\u3400-\u9fff]+")


def emit(**value):
    print(json.dumps(value, ensure_ascii=False), flush=True)


def config_home():
    return Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))


def qt_ini_string(value):
    """Decode QSettings' UTF-16 hex/backslash escapes without eval/codecs eval."""
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    escapes = {
        "n": "\n",
        "r": "\r",
        "t": "\t",
        "a": "\a",
        "b": "\b",
        "f": "\f",
        "v": "\v",
        "\\": "\\",
        '"': '"',
    }

    def replacement(match):
        if match.group(1):
            return chr(int(match.group(1), 16))
        return escapes.get(match.group(2), match.group(2))

    value = re.sub(r'\\x([0-9A-Fa-f]{1,4})|\\([\\"abfnrtv])', replacement, value)
    return value.encode("utf-16", "surrogatepass").decode("utf-16")


def configured_db():
    override = os.environ.get("AI_SPACE_KNOWLEDGE_DB") or os.environ.get("UKUI_FENCES_KNOWLEDGE_DB")
    if override:
        return str(Path(override).expanduser().absolute())
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(str(config_home() / "xikario/ai-space.ini"), encoding="utf-8")
    directory = qt_ini_string(cfg.get("smartSpace", "knowledgeDirectory", fallback=""))
    data = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
    candidate = (
        Path(directory) / "smart-space-knowledge.sqlite"
        if directory
        else data / "ai-space/knowledge/smart-space-knowledge.sqlite"
    )
    if not candidate.is_file():
        old_cfg = configparser.ConfigParser(interpolation=None)
        old_cfg.read(str(config_home() / "kylin/ukui-fences.ini"), encoding="utf-8")
        old_dir = qt_ini_string(
            old_cfg.get(
                "smartSpace", "knowledgeDirectory", fallback=str(data / "ukui-fences/knowledge")
            )
        )
        old = Path(old_dir) / "smart-space-knowledge.sqlite"
        if old.is_file():
            return str(old)
    return str(candidate)


def registry_path():
    return config_home() / "ai-space/libraries.json"


def registry():
    path = registry_path()
    empty = dict(version=1, libraries=[])
    if not path.exists():
        return empty
    if path.stat().st_size > 8 * 1024 * 1024:
        return dict(empty, registry_warnings=["知识库注册表超过读取限制，请修复或恢复备份"], repair_required=True)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, UnicodeError, RecursionError) as error:
        return dict(empty, registry_warnings=["注册表内容损坏：" + str(error)], repair_required=True)
    if (
        not isinstance(value, dict)
        or value.get("version") != 1
        or not isinstance(value.get("libraries"), list)
    ):
        return dict(empty, registry_warnings=["注册表结构或版本无法识别"], repair_required=True)
    valid = []
    warnings = []
    for index, row in enumerate(value["libraries"]):
        try:
            if (
                not isinstance(row, dict)
                or not isinstance(row.get("path"), str)
                or not row["path"]
                or not isinstance(row.get("library_id"), str)
                or not isinstance(row.get("version"), str)
                or not re.fullmatch(r"[A-Za-z0-9._-]{1,80}", row["version"])
                or type(row.get("bytes")) is not int
                or row["bytes"] < 0
                or type(row.get("mtime_ns")) is not int
                or row["mtime_ns"] < 0
                or not isinstance(row.get("read_only"), bool)
                or ("enabled" in row and not isinstance(row["enabled"], bool))
            ):
                raise ValueError("字段无效")
            uuid.UUID(row["library_id"])
            valid.append(row)
        except (ValueError, TypeError, AttributeError):
            warnings.append("第 %d 条记录无效，已暂时跳过" % (index + 1))
    return dict(
        version=1, libraries=valid, registry_warnings=warnings, repair_required=bool(warnings)
    )


def backup_registry(path):
    backup = path.with_name(path.name + ".recovery-" + uuid.uuid4().hex)
    with path.open("rb") as source, backup.open("xb") as target:
        shutil.copyfileobj(source, target, 1024 * 1024)
        target.flush()
        os.fsync(target.fileno())
    os.chmod(backup, 0o600)
    return str(backup)


def repair_registry():
    backup = ""
    with registry_write() as value:
        backup = value.pop("recovery_backup", "")
    return dict(done=True, task="repair-registry", backup=backup, files_deleted=False)


@contextlib.contextmanager
def registry_write():
    path = registry_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(str(path) + ".lock", "a+b") as lock:
        if fcntl is not None:
            fcntl.flock(lock, fcntl.LOCK_EX)
        else:
            import msvcrt

            if lock.seek(0, 2) == 0:
                lock.write(b"0")
                lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        value = registry()
        if value.pop("repair_required", False):
            value["recovery_backup"] = backup_registry(path)
        value.pop("registry_warnings", None)
        yield value
        value.pop("recovery_backup", None)
        fd, temporary = tempfile.mkstemp(prefix=".libraries-", dir=str(path.parent))
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as out:
                json.dump(value, out, ensure_ascii=False, indent=2)
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)


def open_readonly(path):
    path = Path(path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError("Knowledge library not found: " + str(path))
    db = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=1)
    try:
        db.execute("PRAGMA query_only=ON")
        db.execute("PRAGMA trusted_schema=OFF")
        db.execute("PRAGMA cache_size=-8192")
        required = {"documents", "chunks", "chunks_fts", "meta", "extraction_state"}
        tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if not required.issubset(tables):
            raise ValueError("Not a compatible AI Space knowledge database")
        schema = db.execute("SELECT value FROM meta WHERE key='schema_version' LIMIT 1").fetchone()
        if not schema or schema[0] != "2":
            raise ValueError("Unsupported schema; migrate an explicit copy before importing")
        return db
    except BaseException:
        db.close()
        raise


def manifest(db, path, required=False):
    app_id = db.execute("PRAGMA application_id").fetchone()[0]
    if app_id != APPLICATION_ID:
        if required:
            raise ValueError("Select an .aikb file exported by AI Space")
        return dict(
            format="legacy-sqlite",
            format_version=0,
            library_id=dict(db.execute("SELECT key,value FROM meta")).get("library_id")
            or str(uuid.uuid5(uuid.NAMESPACE_URL, str(Path(path).resolve()))),
            version="local-"
            + format(Path(path).stat().st_mtime_ns, "x")
            + "-"
            + format(
                Path(str(path) + "-wal").stat().st_mtime_ns
                if Path(str(path) + "-wal").exists()
                else 0,
                "x",
            ),
            name="个人知识库",
            originals=False,
        )
    row = db.execute("SELECT json FROM aikb_manifest WHERE id=1 AND length(json)<=65536").fetchone()
    if not row or len(row[0]) > 65536:
        raise ValueError("Invalid AIKB manifest")
    value = json.loads(row[0])
    if value.get("format") != "aikb" or value.get("format_version") != FORMAT_VERSION:
        raise ValueError("Unsupported AIKB format version")
    uuid.UUID(value["library_id"])
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,80}", value["version"]):
        raise ValueError("Invalid version")
    if not isinstance(value.get("name"), str):
        raise ValueError("Invalid library name")
    return value


def document_identity(m, path, digest):
    if m.get("format") != "legacy-sqlite":
        return m
    # References survive unrelated commits and a move after identity has been
    # persisted by an explicit build. Changed document text rejects old refs.
    return dict(m, version="doc-" + hashlib.sha256((path + "\0" + digest).encode()).hexdigest())


def availability(db, document_id):
    if not db.execute("SELECT 1 FROM sqlite_master WHERE name='document_availability'").fetchone():
        return "unknown"
    row = db.execute(
        "SELECT status FROM document_availability WHERE document_id=?", (document_id,)
    ).fetchone()
    return row[0] if row else "unknown"


def reference(m, doc_id, chunk_id):
    return "aispace://%s/%s/%d/%d" % (m["library_id"], m["version"], doc_id, chunk_id)


def storage_plan(directory, required_bytes=0, reserve_gib=2, max_gib=32):
    directory = Path(directory).expanduser().absolute()
    parent = directory
    while not parent.exists():
        parent = parent.parent
    if reserve_gib < 0.01 or max_gib < 0.01:
        raise ValueError("Set positive disk reserve and library quota")
    free = shutil.disk_usage(parent).free
    reserve, cap = int(reserve_gib * GIB), int(max_gib * GIB)
    if required_bytes > cap:
        raise StorageLimitError("Library exceeds configured size limit")
    if free - required_bytes < reserve:
        raise StorageLimitError(
            "Insufficient disk space: need %d bytes plus %d reserved bytes; available %d"
            % (required_bytes, reserve, free)
        )
    return dict(
        directory=str(directory),
        freeBytes=free,
        requiredBytes=required_bytes,
        reserveBytes=reserve,
        maxBytes=cap,
    )


def guard_storage(path, reserve_gib=2, max_gib=32):
    path = Path(path)
    size = sum(p.stat().st_size for p in (path, Path(str(path) + "-wal")) if p.exists())
    if size > int(max_gib * GIB):
        raise StorageLimitError("Library reached configured size limit (including WAL)")
    return storage_plan(path.parent, 0, reserve_gib, max_gib)


def ignored_ids(db):
    if not db.execute("SELECT 1 FROM sqlite_master WHERE name='ignored_documents'").fetchone():
        return set()
    return {row[0] for row in db.execute("SELECT document_id FROM ignored_documents")}


def document_ids(value):
    ids = json.loads(value)
    if (
        not isinstance(ids, list)
        or not ids
        or len(ids) > 200
        or any(type(i) is not int or i <= 0 for i in ids)
    ):
        raise ValueError("Select between 1 and 200 document IDs")
    return sorted(set(ids))


def describe_issue(value):
    """Localize presentation only; stored issue codes remain machine-readable."""
    text = str(value).strip()
    exact = {
        "previous_chunks_retained": "此前已入库的正文（如有）已保留",
        "workbook is encrypted": "工作簿已加密，无法读取；请解除加密后重新处理",
        "encrypted word document": "Word 文档已加密，无法读取；请解除加密后重新处理",
        "file is not a zip file": "文件内容不是有效的压缩文档，可能已损坏或扩展名不符；请用办公软件检查并另存后重试",
        "workbook_has_no_worksheets": "工作簿中没有可读取的工作表",
        "word piece table missing": "Word 文档缺少正文索引；请用办公软件修复或另存为 .docx 后重试",
        "invalid word piece table": "Word 文档的正文索引无效；请用办公软件修复或另存为 .docx 后重试",
        "truncated word fib": "Word 文档头不完整，文件可能已截断；请检查或重新保存",
        "truncated word text piece": "Word 正文数据不完整，文件可能已截断；请检查或重新保存",
        "unsupported word fib version": "暂不支持此版本的旧版 Word 格式；请另存为 .docx 后重试",
        "source changed during ocr; cached pages not reused": "识别过程中源文件发生变化，已停止复用旧页面缓存；请待文件保存完成后重试",
        "retry produced no new text; previous indexed body retained": "本次重新处理未提取到新正文，此前已入库的正文已保留",
        "image_needs_ocr": "图片尚需进行文字识别，请运行 OCR 补全",
        "slide_relationships_missing:natural_order_fallback": "演示文稿缺少页面顺序信息，已按页面编号顺序提取",
        "legacy_word:main_text_only;layout_not_preserved": "旧版 Word 文档仅提取主要正文，未保留原始排版",
        "large_log_summary:head_errors_context_tail": "日志较大，仅提取开头、错误及其上下文和末尾内容",
        "odf:paragraph_locations_only": "开放文档按段落提取，位置信息仅精确到段落",
        "legacy_ppt:slide_text_only": "已提取旧版演示文稿的页面文字；未提取备注、图片中的文字及嵌入对象内容",
    }
    if text.casefold() in exact:
        return exact[text.casefold()]
    # Older records sometimes combine the error and retention marker in one
    # string. Translate each part, without rewriting the database.
    if ";" in text:
        return "；".join(
            dict.fromkeys(describe_issue(part) for part in text.split(";") if part.strip())
        )
    lower = text.casefold()
    if lower.startswith("ocr_languages_missing:"):
        return (
            "缺少 OCR 语言包："
            + text.partition(":")[2]
            + "；请安装对应 Tesseract 语言包，或在建库向导修改 OCR 语言（简体中文：tesseract-ocr-chi-sim）"
        )
    if lower == "ocr_language_probe_failed":
        return "无法读取 OCR 语言包列表，请检查 Tesseract 安装"
    if lower == "ocr_languages_invalid":
        return "OCR 语言配置无效；请使用 chi_sim、eng 等语言代码，并用 + 连接"
    if "incorrect password" in lower or "password required" in lower:
        return "PDF 文件已加密或需要密码；请解除加密后重新处理"
    if lower.startswith("document_text_limit:"):
        return "正文达到单文档提取上限，已保留上限内的内容；此文件仅部分入库"
    if lower == "document_input_limit":
        return "文件达到单文档读取上限，仅提取已读取范围；此文件仅部分入库"
    if lower == "decode_errors_replaced":
        return "文件含无法解码的字节，已替换异常字符并保留其余正文"
    if lower == "rtf:visible_text_only":
        return "RTF 仅提取可见文字，未保留排版和嵌入对象"
    if "binary content" in lower:
        return "文件实际包含二进制内容或视频，不能按纯文本提取；请检查文件类型"
    if lower.startswith("unsupported container:"):
        kind = lower.partition(":")[2].strip()
        if kind == "ppt-binary":
            return "此前的提取器未支持旧版 PowerPoint（.ppt）；请重新处理，尝试提取页面文字"
        if kind == "ole-unknown":
            return "无法识别此旧版办公文件的内部结构；请用办公软件检查，并另存为新版格式后重试"
        if kind == "zip-unknown":
            return "压缩文档中未找到受支持的办公正文结构；请检查文件格式并重新保存"
        return "暂不支持此文件的实际格式，或文件内容与扩展名不符；请用办公软件检查并另存后重试"
    for prefix, description in (
        ("page_needs_ocr:", "第 %s 页尚需文字识别，请运行 OCR 补全"),
        ("empty_ocr_page:", "第 %s 页已完成文字识别，但未识别出正文"),
        ("legacy_ppt_page_without_text:", "第 %s 页未提取到文字，可能是图片或空白页；如需识别，请转换为 PDF 后进行 OCR"),
    ):
        if lower.startswith(prefix):
            return description % text[len(prefix) :]
    if lower.startswith(("missing_slide:", "missing_slide_relationship:")):
        return "演示文稿缺少页面数据或关联信息，部分内容未能提取；请用办公软件检查并另存后重试"
    if lower.startswith("nonstandard_body_element:"):
        return "正文包含暂不支持的特殊结构，部分内容可能未提取"
    if lower.startswith("cell_format_unresolved:"):
        return "单元格格式无法完整解析，部分显示格式未保留"
    if lower.startswith("missing worksheet relationship:"):
        return "工作簿缺少工作表关联信息；请另存为 .xlsx 后重新处理"
    if lower.startswith("shared string index out of bounds:"):
        return "工作簿的共享文本索引无效；请用办公软件修复或另存后重试"
    if lower.startswith("workbook corruption:"):
        return "工作簿内部结构损坏；请用办公软件修复或另存后重新处理"
    if "not installed" in lower or lower.startswith("no module named"):
        tools = [
            name
            for token, name in (
                ("pdftotext", "PDF 文本提取工具（pdftotext）"),
                ("pdftoppm", "PDF 页面转换工具（pdftoppm）"),
                ("tesseract", "文字识别工具（Tesseract）"),
                ("olefile", "旧版办公文件读取组件（olefile）"),
                ("xlrd", "旧版工作簿读取组件（xlrd）"),
            )
            if token in lower
        ]
        return "缺少" + ("、".join(tools) if tools else "所需的文件解析组件") + "；请安装后重新处理"
    tool_error = re.fullmatch(r"(pdftotext|pdftoppm|tesseract) failed \((-?\d+)\)", lower)
    if tool_error:
        operation = {"pdftotext": "PDF 文本提取", "pdftoppm": "PDF 页面转换", "tesseract": "图片文字识别"}[
            tool_error[1]
        ]
        return "%s失败（退出码 %s）；请检查源文件是否可正常打开后重试" % (operation, tool_error[2])
    if lower.startswith("pdf page rendering failed:"):
        return "PDF 第 %s 页转换为图片失败；请检查文件后重试" % text.partition(":")[2].strip()
    if "no such file or directory" in lower or lower == "filenotfounderror":
        return "找不到来源文件；请确认文件仍在原位置、存储设备已连接后重新处理"
    if "permission denied" in lower or lower == "permissionerror":
        return "没有读取文件的权限；请检查文件访问权限后重新处理"
    if "timed out" in lower or "timeout" in lower:
        return "文件处理超时，已停止本次提取；可稍后重新处理"
    if "codec can't decode" in lower or "unicodedecodeerror" in lower:
        return "文件文字编码无法正确解析；请另存为 UTF-8 编码或新版办公格式后重试"
    if "no space left" in lower or "database or disk is full" in lower:
        return "存储空间不足，无法继续写入；请释放空间后重新处理"
    if lower == "memoryerror" or "cannot allocate memory" in lower:
        return "可用内存不足，未能完成处理；请关闭高占用程序后重试"
    if "encrypted" in lower or "password" in lower:
        return "文件已加密或需要密码，无法读取；请解除加密后重新处理"
    if "crc" in lower or "badzipfile" in lower or "bad magic number" in lower:
        return "文件内部数据校验失败，可能已损坏；请检查或重新保存文件后重试"
    if any(
        token in lower
        for token in (
            "not well-formed",
            "unclosed token",
            "no element found",
            "mismatched tag",
            "syntax error: line",
        )
    ):
        return "文档内部结构无法解析，可能存在损坏；请用办公软件修复或另存后重试"
    if re.search(r"[\u3400-\u9fff]", text):
        return text
    return "未能解析此文件，暂未识别出具体原因；可重新处理，或将无需处理的文件设为忽略"


def issue_fields(values, limit=1500):
    if not isinstance(values, list):
        values = [values]
    descriptions = list(
        dict.fromkeys(describe_issue(value) for value in values if str(value).strip())
    )
    return dict(
        reason=("；".join(descriptions) or "未记录具体原因；可重新处理以更新诊断")[:limit],
        technical_reason="; ".join(map(str, values))[:limit],
        issueDescriptions=descriptions,
    )


FAILURE_CATEGORIES = {
    "all": "全部原因",
    "encrypted": "文件加密",
    "unsupported": "格式不支持",
    "missing": "来源缺失",
    "corrupt": "文件损坏 / 结构异常",
    "extraction": "文本提取失败",
    "ocr": "OCR / 页面转换失败",
    "tools": "缺少解析工具",
    "permissions": "权限不足",
    "encoding": "文字编码异常",
    "timeout": "处理超时",
    "resources": "资源不足",
    "other": "其他问题",
}


def failure_category(status, issues):
    """Classify diagnostics, never infer corruption just from a tool exit."""
    if not isinstance(issues, list):
        issues = [issues]
    text = "; ".join(map(str, issues)).casefold()
    if (
        status == "missing"
        or "no such file or directory" in text
        or "找不到来源文件" in text
        or "source missing" in text
    ):
        return "missing"
    if "permission denied" in text or "permissionerror" in text or "没有读取文件的权限" in text:
        return "permissions"
    if (
        "not installed" in text
        or "no module named" in text
        or "缺少解析工具" in text
        or "ocr_languages_missing:" in text
        or "tesseract not installed" in text
    ):
        return "tools"
    if "timed out" in text or "timeout" in text or "超时" in text:
        return "timeout"
    if any(
        v in text
        for v in (
            "no space left",
            "disk is full",
            "memoryerror",
            "cannot allocate memory",
            "内存不足",
            "空间不足",
            "资源超限",
            "解析限额",
            "时间限额",
            "内存限额",
        )
    ):
        return "resources"
    if "codec can't decode" in text or "unicodedecodeerror" in text or "文字编码" in text:
        return "encoding"
    if (
        re.search(r"\b(?:encrypted|password[- ]protected)\b", text)
        or "incorrect password" in text
        or "password required" in text
        or "已加密" in text
        or "需要密码" in text
    ):
        return "encrypted"
    if (
        "binary content" in text
        or "unsupported container:" in text
        or "unsupported word fib version" in text
        or "暂不支持" in text
    ):
        return "unsupported"
    if any(
        v in text
        for v in (
            "tesseract failed",
            "pdftoppm failed",
            "pdf page rendering failed",
            "文字识别失败",
            "页面转换失败",
        )
    ):
        return "ocr"
    if "pdftotext failed" in text or "文本提取失败" in text:
        return "extraction"
    if any(
        v in text
        for v in (
            "corruption",
            "file is not a zip file",
            "badzipfile",
            "crc",
            "truncated",
            "missing worksheet relationship:",
            "shared string index out of bounds:",
            "word piece table missing",
            "invalid word piece table",
            "not well-formed",
            "mismatched tag",
            "unclosed token",
            "no element found",
            "损坏",
            "结构无效",
            "文件标识无效",
            "不完整",
            "引用无效",
            "引用超出范围",
            "文件边界",
        )
    ):
        return "corrupt"
    return "other"


def _indexed_failure_scope(db, path, ignored, category, offset, limit, collect_all):
    columns = {r[1] for r in db.execute("PRAGMA table_info(extraction_state)")}
    if "failure_category" not in columns:
        return None
    revision = dict(
        db.execute("SELECT key,value FROM meta WHERE key IN ('failure_revision','failure_epoch')")
    )
    if len(revision) != 2:
        return None
    if db.execute(
        "SELECT 1 FROM extraction_state WHERE failure_category='' OR failure_category IS NULL LIMIT 1"
    ).fetchone():
        return None
    has_policy = db.execute("SELECT 1 FROM sqlite_master WHERE name='ignored_documents'").fetchone()
    join = "LEFT JOIN ignored_documents i ON i.document_id=e.document_id" if has_policy else ""
    predicate = (
        (
            "i.document_id IS NOT NULL"
            if ignored
            else "e.status IN ('failed','missing') AND i.document_id IS NULL"
        )
        if has_policy
        else ("0" if ignored else "e.status IN ('failed','missing')")
    )
    counts = {key: 0 for key in FAILURE_CATEGORIES}
    for key, count in db.execute(
        "SELECT e.failure_category,count(*) FROM extraction_state e "
        + join
        + " WHERE "
        + predicate
        + " GROUP BY e.failure_category"
    ):
        counts[key if key in counts else "other"] += count
        counts["all"] += count
    total = counts[category]
    where = predicate + (" AND e.failure_category=?" if category != "all" else "")
    args = [category] if category != "all" else []
    fields = "d.id,substr(d.name,1,4096),substr(d.path,1,32768),e.status,substr(e.issues_json,1,16384),e.attempted_at,e.source_signature,e.failure_category"
    query = (
        " FROM extraction_state e JOIN documents d ON d.id=e.document_id "
        + join
        + " WHERE "
        + where
        + " ORDER BY e.document_id"
    )
    rows = []
    for row in db.execute("SELECT " + fields + query + " LIMIT ? OFFSET ?", args + [limit, offset]):
        try:
            issues = json.loads(row[4])
        except (ValueError, TypeError):
            issues = ["无法读取旧失败记录"]
        rows.append((row[:7], issues, row[7]))
    ids = [
        r[0]
        for r in db.execute(
            "SELECT e.document_id" + query + ("" if collect_all else " LIMIT 201"), args
        )
    ]
    # Every relevant write increments the revision, even replacement by another
    # ID with the same count/timestamp. The epoch detects a replaced database.
    token = hashlib.sha256(
        json.dumps(
            [str(Path(path).resolve()), ignored, category, revision, total], sort_keys=True
        ).encode()
    ).hexdigest()
    return dict(
        total=total,
        rows=rows,
        ids=ids,
        selection_token=token,
        categories=[dict(id=k, label=v, count=counts[k]) for k, v in FAILURE_CATEGORIES.items()],
    )


def _failure_scope(db, path, ignored, category, offset=0, limit=50, collect_all=False):
    if category not in FAILURE_CATEGORIES:
        raise ValueError("未知的失败分类，请刷新列表")
    fast = _indexed_failure_scope(db, path, ignored, category, offset, limit, collect_all)
    if fast is not None:
        return fast
    has_policy = db.execute("SELECT 1 FROM sqlite_master WHERE name='ignored_documents'").fetchone()
    join = "LEFT JOIN ignored_documents i ON i.document_id=d.id" if has_policy else ""
    predicate = (
        (
            "i.document_id IS NOT NULL"
            if ignored
            else "e.status IN ('failed','missing') AND i.document_id IS NULL"
        )
        if has_policy
        else ("0" if ignored else "e.status IN ('failed','missing')")
    )
    sql = (
        "SELECT d.id,substr(d.name,1,4096),substr(d.path,1,32768),e.status,substr(e.issues_json,1,16384),e.attempted_at,e.source_signature FROM documents d JOIN extraction_state e ON e.document_id=d.id "
        + join
        + " WHERE "
        + predicate
        + " ORDER BY d.id"
    )
    counts = {key: 0 for key in FAILURE_CATEGORIES}
    rows = []
    ids = []
    total = 0
    digest = hashlib.sha256(json.dumps([str(Path(path).resolve()), ignored, category]).encode())
    heartbeat = time.monotonic()
    for row in db.execute(sql):
        if time.monotonic() - heartbeat > 1:
            emit(task="failures", phase="classify-legacy", processed=counts["all"])
            heartbeat = time.monotonic()
        try:
            issues = json.loads(row[4])
        except (ValueError, TypeError):
            issues = ["无法读取旧失败记录"]
        kind = failure_category(row[3], issues)
        counts["all"] += 1
        counts[kind] += 1
        if category != "all" and category != kind:
            continue
        digest.update(json.dumps(row, ensure_ascii=False).encode())
        if collect_all or len(ids) < 201:
            ids.append(row[0])
        if offset <= total < offset + limit:
            rows.append((row, issues, kind))
        total += 1
    return dict(
        total=total,
        rows=rows,
        ids=ids,
        selection_token=digest.hexdigest(),
        categories=[
            dict(id=key, label=label, count=counts[key])
            for key, label in FAILURE_CATEGORIES.items()
        ],
    )


def failure_list(path, ignored=False, offset=0, limit=50, category="all"):
    offset = max(0, offset)
    limit = max(1, min(limit, 100))
    with contextlib.closing(open_readonly(path)) as db:
        db.execute("BEGIN")
        identity = manifest(db, path)
        scope = _failure_scope(db, path, ignored, category, offset, limit)
        result = []
        for row, reasons, kind in scope["rows"]:
            ident, name, source, state, _issues, attempted, _signature = row
            result.append(
                dict(
                    document_id=ident,
                    name=name,
                    path=source,
                    status=state,
                    attempted_at=attempted,
                    ignored=ignored,
                    failure_category=kind,
                    failure_category_label=FAILURE_CATEGORIES[kind],
                    **issue_fields(reasons),
                )
            )
        return dict(
            done=True,
            task="failures",
            path=str(Path(path).absolute()),
            writable=identity.get("format") == "legacy-sqlite",
            total=scope["total"],
            offset=offset,
            category=category,
            categories=scope["categories"],
            selection_token=scope["selection_token"],
            selection_ids=scope["ids"] if scope["total"] <= 200 else [],
            results=result,
        )


def set_ignored_matching(path, category, ignored, selection_token, expected_count):
    """One atomic action for exactly the previously selected filtered scope."""
    path = Path(path).expanduser().resolve()
    if (
        not isinstance(selection_token, str)
        or not re.fullmatch(r"[a-f0-9]{64}", selection_token)
        or expected_count <= 0
    ):
        raise ValueError("请先全选当前筛选结果，再执行批量操作")
    with contextlib.closing(open_readonly(path)) as check:
        if manifest(check, path).get("format") != "legacy-sqlite":
            raise ValueError("共享 AIKB 知识库只读，请在资料来源机器处理")
    try:
        from smart_space_knowledge import build_lock
    except ImportError:
        raise ValueError("批量处理需要安装完整的 AI空间应用")
    with build_lock(str(path)), contextlib.closing(sqlite3.connect(str(path), timeout=1)) as db:
        if db.execute("PRAGMA application_id").fetchone()[0] == APPLICATION_ID:
            raise ValueError("共享 AIKB 知识库只读")
        db.execute("PRAGMA foreign_keys=ON")
        db.execute("BEGIN IMMEDIATE")
        scope = _failure_scope(db, path, not ignored, category, limit=0, collect_all=True)
        if scope["selection_token"] != selection_token or scope["total"] != expected_count:
            raise ValueError("筛选结果已变化，未更改任何文件；请刷新后重新全选")
        db.execute(
            "CREATE TABLE IF NOT EXISTS ignored_documents(document_id INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,ignored_at TEXT NOT NULL)"
        )
        if ignored:
            db.executemany(
                "INSERT OR REPLACE INTO ignored_documents VALUES(?,?)",
                ((ident, time.strftime("%Y-%m-%dT%H:%M:%S")) for ident in scope["ids"]),
            )
        else:
            db.executemany(
                "DELETE FROM ignored_documents WHERE document_id=?",
                ((ident,) for ident in scope["ids"]),
            )
        db.commit()
    return dict(
        done=True,
        task="failure-policy",
        ignored=ignored,
        changed=expected_count,
        body_preserved=True,
    )


def set_ignored(path, ids, ignored):
    path = Path(path).expanduser().resolve()
    ids = document_ids(json.dumps(ids))
    # Share the builder's lock, but do not run schema migrations or rebuild FTS
    # merely to change a failure preference.
    with contextlib.closing(open_readonly(path)) as check:
        if manifest(check, path).get("format") != "legacy-sqlite":
            raise ValueError(
                "Shared AIKB libraries are read-only; manage failures on the source machine"
            )
    try:
        from smart_space_knowledge import build_lock
    except ImportError:
        raise ValueError("Failure preferences require the installed AI Space application runtime")
    with build_lock(str(path)), contextlib.closing(sqlite3.connect(str(path), timeout=1)) as db:
        if db.execute("PRAGMA application_id").fetchone()[0] == APPLICATION_ID:
            raise ValueError("AIKB libraries are read-only")
        db.execute("PRAGMA foreign_keys=ON")
        slots = ",".join("?" for _ in ids)
        rows = db.execute(
            "SELECT d.id,e.status FROM documents d JOIN extraction_state e ON e.document_id=d.id WHERE d.id IN ("
            + slots
            + ")",
            ids,
        ).fetchall()
        if len(rows) != len(ids) or (
            ignored and any(state not in ("failed", "missing") for ident, state in rows)
        ):
            raise ValueError("Selected failure records changed; refresh the list before retrying")
        db.execute(
            "CREATE TABLE IF NOT EXISTS ignored_documents(document_id INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,ignored_at TEXT NOT NULL)"
        )
        if ignored:
            db.executemany(
                "INSERT OR REPLACE INTO ignored_documents VALUES(?,?)",
                [(ident, time.strftime("%Y-%m-%dT%H:%M:%S")) for ident in ids],
            )
        else:
            db.executemany(
                "DELETE FROM ignored_documents WHERE document_id=?", [(ident,) for ident in ids]
            )
        db.commit()
    return dict(
        done=True, task="failure-policy", ignored=ignored, changed=len(ids), body_preserved=True
    )


def stats(path, details=False, fast=False):
    with contextlib.closing(open_readonly(path)) as db:
        db.execute("BEGIN")  # One consistent snapshot while another worker commits.
        m = manifest(db, path)
        result = dict(
            done=True,
            task="stats",
            path=str(Path(path).resolve()),
            manifest=m,
            documents=db.execute("SELECT count(*) FROM documents").fetchone()[0],
            chunks=db.execute("SELECT count(*) FROM chunks").fetchone()[0],
            extractionStates=dict(
                db.execute("SELECT status,count(*) FROM extraction_state GROUP BY status")
            ),
            dbBytes=sum(
                p.stat().st_size for p in (Path(path), Path(str(path) + "-wal")) if p.exists()
            ),
            lastBuild=dict(db.execute("SELECT key,value FROM meta")).get("last_build_at", ""),
        )
        if details:
            result["ignoredDocuments"] = db.execute(
                "SELECT count(*) FROM ignored_documents"
            ).fetchone()[0]
            result["failurePending"] = 0
            result["retainedAbsent"] = (
                db.execute(
                    "SELECT count(*) FROM document_availability WHERE status='unseen'"
                ).fetchone()[0]
                if db.execute(
                    "SELECT 1 FROM sqlite_master WHERE name='document_availability'"
                ).fetchone()
                else 0
            )
            checkpoint = Path(path).parent / "catalog/job.json"
            try:
                result["checkpoint"] = (
                    json.loads(checkpoint.read_text()) if checkpoint.stat().st_size < 16384 else {}
                )
            except (OSError, ValueError):
                result["checkpoint"] = {}
            pending_docs = pending_pages = pending_images = empty_pages = empty_images = 0
            failures = []
            summary_exists = db.execute(
                "SELECT 1 FROM sqlite_master WHERE name='extraction_summary'"
            ).fetchone()
            summary_ready = (
                bool(summary_exists)
                and not db.execute(
                    "SELECT 1 FROM extraction_state e LEFT JOIN extraction_summary s ON s.document_id=e.document_id WHERE s.document_id IS NULL LIMIT 1"
                ).fetchone()
            )
            pending_complete = summary_ready or not fast
            if summary_ready:
                row = db.execute(
                    """SELECT coalesce(sum(CASE WHEN i.document_id IS NULL AND (s.pending_pages>0 OR s.pending_images>0) THEN 1 ELSE 0 END),0),
                    coalesce(sum(CASE WHEN i.document_id IS NULL THEN s.pending_pages ELSE 0 END),0),
                    coalesce(sum(CASE WHEN i.document_id IS NULL THEN s.pending_images ELSE 0 END),0),
                    coalesce(sum(s.empty_pages),0),coalesce(sum(s.empty_images),0)
                    FROM extraction_summary s LEFT JOIN ignored_documents i ON i.document_id=s.document_id"""
                ).fetchone()
                pending_docs, pending_pages, pending_images, empty_pages, empty_images = row
            elif not fast:
                skipped = ignored_ids(db)
                for ident, name, source, status, issues, kind in db.execute(
                    "SELECT d.id,d.name,d.path,e.status,e.issues_json,e.source_kind FROM extraction_state e JOIN documents d ON d.id=e.document_id WHERE e.status!='complete'"
                ):
                    try:
                        values = json.loads(issues)
                    except (ValueError, TypeError):
                        values = []
                    pages = {str(v) for v in values if str(v).startswith("page_needs_ocr:")}
                    image = "image_needs_ocr" in values
                    empty_pages += len(
                        {str(v) for v in values if str(v).startswith("empty_ocr_page:")}
                    )
                    empty_images += int(kind == "image" and status == "no_text")
                    if (pages or image) and ident not in skipped:
                        pending_docs += 1
                        pending_pages += len(pages)
                        pending_images += int(image)
            result["failurePending"] = db.execute(
                "SELECT count(*) FROM extraction_state e WHERE e.status IN ('failed','missing') AND NOT EXISTS(SELECT 1 FROM ignored_documents i WHERE i.document_id=e.document_id)"
            ).fetchone()[0]
            for ident, name, source, status, issues in db.execute(
                "SELECT d.id,d.name,d.path,e.status,e.issues_json FROM extraction_state e JOIN documents d ON d.id=e.document_id WHERE e.status IN ('failed','missing') AND NOT EXISTS(SELECT 1 FROM ignored_documents i WHERE i.document_id=e.document_id) ORDER BY e.document_id LIMIT 20"
            ):
                try:
                    values = json.loads(issues)
                except (ValueError, TypeError):
                    values = []
                failures.append(
                    dict(
                        document_id=ident,
                        name=name,
                        path=source,
                        status=status,
                        **issue_fields(values, 500),
                    )
                )
            result["ocrPending"] = dict(
                complete=pending_complete,
                statisticsSource="summary"
                if summary_ready
                else "legacy-scan"
                if not fast
                else "awaiting-upgrade",
                documents=pending_docs if pending_complete else None,
                pdfPages=pending_pages if pending_complete else None,
                images=pending_images if pending_complete else None,
                emptyOcrPages=empty_pages if pending_complete else None,
                imagesWithoutText=empty_images if pending_complete else None,
                scope="已提取文档中的明确待OCR标记；未检测文件不计入，非完整扫描任务队列",
            )
            result["failures"] = failures
        return result


def _snippet(content, query):
    pieces = SEGMENT.findall(query)
    positions = [content.casefold().find(p.casefold()) for p in pieces]
    positions = [p for p in positions if p >= 0]
    start = max(0, min(positions) - 80) if positions else 0
    return ("…" if start else "") + content[start : start + 650]


def search_one(path, query, limit=20, budget_ms=1800, body_only=False):
    if len(query) > 1000:
        raise ValueError("请使用不超过1000字的关键词查询")
    started = time.monotonic()
    deadline = started + max(10, min(budget_ms, 30000)) / 1000
    limit = max(1, min(int(limit), 200))
    segments = SEGMENT.findall(query)[:12]
    phrases = [" ".join(TOKEN.findall(s.casefold())) for s in segments]
    phrases = [p for p in phrases if p]
    unsupported = any(
        c.isalpha() and ord(c) > 127 and not ("\u3400" <= c <= "\u9fff") for c in query
    )
    output = dict(
        results=[],
        timed_out=False,
        truncated=False,
        lowConfidence=False,
        body_searched=False,
        unsupported_language=unsupported,
    )
    if not phrases:
        return output
    with contextlib.closing(open_readonly(path)) as db:
        m = manifest(db, path)
        db.set_progress_handler(lambda: int(time.monotonic() > deadline), 1000)
        # Iterate ranked chunk IDs lazily and skip repeated documents before
        # loading raw text. A long report must not fill every candidate slot.
        sql = "SELECT chunk_id,document_id,rank FROM chunks_fts WHERE chunks_fts MATCH ? AND rank MATCH 'bm25(8.0,3.0,1.0)' ORDER BY rank LIMIT ?"
        candidate_cap = 4096
        try:
            seen = set()
            # Metadata is small (one row/document), unlike millions of body
            # chunks. Provide useful filename hits even for extremely common
            # words whose full-text ranking may exceed the interactive budget.
            predicates = " AND ".join("instr(lower(d.name),?)>0" for _ in segments)
            title_arguments = [s.casefold() for s in segments]
            if db.execute("SELECT 1 FROM sqlite_master WHERE name='document_names_fts'").fetchone():
                expression = " AND ".join(
                    '"' + " ".join(c for c in segment.casefold() if c.isalnum()) + '"'
                    for segment in segments
                    if any(c.isalnum() for c in segment)
                )
                # Index narrows candidates, then the original substring check
                # preserves filename matching semantics. Old read-only packages
                # continue through the deadline-bounded compatibility path.
                predicates = (
                    "d.id IN (SELECT rowid FROM document_names_fts WHERE document_names_fts MATCH ?) AND "
                    + predicates
                )
                title_arguments.insert(0, expression)
            title_sql = (
                """SELECT d.id,substr(d.path,1,32768),substr(d.name,1,4096),substr(d.category,1,128),d.modified_ms,d.size,
                (SELECT id FROM chunks WHERE document_id=d.id ORDER BY ordinal LIMIT 1),e.status,d.content_hash
                FROM documents d LEFT JOIN extraction_state e ON e.document_id=d.id WHERE """
                + predicates
                + " LIMIT ?"
            )
            for doc_id, origin, name, category, modified, size, chunk_id, state, digest in (
                [] if body_only else db.execute(title_sql, title_arguments + [limit])
            ):
                seen.add(doc_id)
                identity = document_identity(m, origin, digest)
                has_body = chunk_id is not None
                chunk_id = chunk_id or 0
                output["results"].append(
                    dict(
                        path=origin,
                        originalPath=origin,
                        name=name,
                        category=category,
                        modifiedMs=modified,
                        size=size,
                        locator="",
                        snippet="文件名匹配 · 请读取库内正文核对" if has_body else "仅有文件名与元数据 · 当前库没有可用正文",
                        hasBody=has_body,
                        document_id=doc_id,
                        chunk_id=chunk_id,
                        ordinal=0,
                        library_id=m["library_id"],
                        library_version=identity["version"],
                        library_name=m["name"],
                        reference=reference(identity, doc_id, chunk_id),
                        sourceAvailability=availability(db, doc_id),
                        source_db=str(Path(path).absolute()),
                        extractionStatus=state or "legacy",
                        matchedSegments=segments,
                        lowConfidence=False,
                        score=85,
                        relaxed=False,
                        match_scope="filename",
                    )
                )
            hit_count = 0
            if len(output["results"]) >= limit:
                output["truncated"] = True
                output["library_id"] = m["library_id"]
                output["elapsed_ms"] = round((time.monotonic() - started) * 1000, 2)
                return output
            output["body_searched"] = True
            passes = [(False, " AND ".join('"' + p + '"' for p in phrases), None)]
            if len(phrases) > 1:
                passes.append((True, " OR ".join('"' + p + '"' for p in phrases), None))
            elif len(segments[0]) > 2 and re.fullmatch(r"[\u3400-\u9fff]+", segments[0]):
                text = segments[0]
                grams = list(dict.fromkeys(text[i : i + 2] for i in range(len(text) - 1)))[:40]
                passes.append((True, " OR ".join('"' + " ".join(g) + '"' for g in grams), grams))
            for relaxed, expression, grams in passes:
                if relaxed and hit_count:
                    break
                for chunk_id, advertised_document, rank in db.execute(
                    sql, (expression, candidate_cap)
                ):
                    hit_count += 1
                    if time.monotonic() > deadline:
                        output["timed_out"] = True
                        break
                    if advertised_document in seen:
                        continue
                    row = db.execute(
                        """SELECT d.id,substr(d.path,1,32768),substr(d.name,1,4096),substr(d.category,1,128),d.modified_ms,d.size,
                        c.ordinal,substr(c.locator,1,4096),substr(c.content,1,32768),e.status,d.content_hash
                        FROM chunks c JOIN documents d ON d.id=c.document_id
                        LEFT JOIN extraction_state e ON e.document_id=d.id WHERE c.id=?""",
                        (chunk_id,),
                    ).fetchone()
                    if not row or row[0] != advertised_document:
                        continue
                    seen.add(row[0])
                    doc_id, origin, name, category, modified, size, ordinal, locator, content, state, digest = (
                        row
                    )
                    identity = document_identity(m, origin, digest)
                    haystack = re.sub(r"\s+", "", name + " " + content).casefold()
                    coverage_terms = grams or segments
                    matched = [
                        s for s in coverage_terms if re.sub(r"\s+", "", s).casefold() in haystack
                    ]
                    if grams and len(matched) < max(2, (len(grams) + 1) // 2):
                        continue
                    low = not matched
                    output["results"].append(
                        dict(
                            path=origin,
                            originalPath=origin,
                            name=name,
                            category=category,
                            modifiedMs=modified,
                            size=size,
                            locator=locator,
                            snippet=_snippet(content, " ".join(matched) if grams else query),
                            document_id=doc_id,
                            chunk_id=chunk_id,
                            ordinal=ordinal,
                            library_id=m["library_id"],
                            library_version=identity["version"],
                            library_name=m["name"],
                            reference=reference(identity, doc_id, chunk_id),
                            sourceAvailability=availability(db, doc_id),
                            source_db=str(Path(path).absolute()),
                            extractionStatus=state or "legacy",
                            matchedSegments=matched,
                            lowConfidence=low,
                            score=min(
                                90 if relaxed else 99,
                                40
                                + int(50 * len(matched) / max(1, len(coverage_terms)))
                                + (5 if query in name else 0),
                            )
                            if not low
                            else 30,
                            relaxed=relaxed,
                            match_scope="body",
                        )
                    )
                    if len(output["results"]) >= limit:
                        output["truncated"] = True
                        break
                if output["timed_out"] or output["truncated"]:
                    break
            output["truncated"] = output["truncated"] or hit_count >= candidate_cap
            output["lowConfidence"] = bool(output["results"]) and all(
                r["lowConfidence"] for r in output["results"]
            )
        except sqlite3.OperationalError as exc:
            if "interrupted" not in str(exc):
                raise
            output["timed_out"] = True
        finally:
            db.set_progress_handler(None, 0)
        output["library_id"] = m["library_id"]
    output["elapsed_ms"] = round((time.monotonic() - started) * 1000, 2)
    return output


def fetch(
    path,
    doc_id=None,
    chunk_id=None,
    original_path=None,
    limit=8,
    offset=0,
    neighbors=1,
    max_chars=32768,
):
    with contextlib.closing(open_readonly(path)) as db:
        m = manifest(db, path)
        if original_path:
            row = db.execute("SELECT id FROM documents WHERE path=?", (original_path,)).fetchone()
            if not row:
                raise ValueError("Document not found in library")
            doc_id = row[0]
        doc = db.execute(
            "SELECT substr(name,1,4096),substr(path,1,32768),content_hash FROM documents WHERE id=?",
            (doc_id,),
        ).fetchone()
        if not doc:
            raise ValueError("Document not found in library")
        if chunk_id not in (None, 0):
            hit = db.execute(
                "SELECT ordinal FROM chunks WHERE id=? AND document_id=?", (chunk_id, doc_id)
            ).fetchone()
            if not hit:
                raise ValueError("Chunk does not belong to document")
            offset = max(0, hit[0] - max(0, min(neighbors, 5)))
        rows = db.execute(
            "SELECT id,ordinal,substr(locator,1,4096),substr(content,1,32768),length(content)>32768 FROM chunks WHERE document_id=? AND ordinal>=? ORDER BY ordinal LIMIT ?",
            (doc_id, max(0, offset), max(1, min(limit, 30)) + 1),
        ).fetchall()
        m = document_identity(m, doc[1], doc[2])
        budget = max(1024, min(int(max_chars), 131072))
        selected = []
        for row in rows[: max(1, min(limit, 30))]:
            if selected and len(row[3]) > budget:
                break
            selected.append(
                (row[0], row[1], row[2], row[3][:budget], row[4] or len(row[3]) > budget)
            )
            budget -= len(selected[-1][3])
            if budget <= 0:
                break
        state = db.execute(
            "SELECT status FROM extraction_state WHERE document_id=?", (doc_id,)
        ).fetchone()
        return dict(
            done=True,
            task="fetch",
            name=doc[0],
            originalPath=doc[1],
            library_id=m["library_id"],
            document_id=doc_id,
            extractionStatus=state[0] if state else "legacy",
            hasBody=bool(rows),
            content_trust="untrusted_document",
            sourceAvailability=availability(db, doc_id),
            hasMore=len(rows) > len(selected),
            nextOffset=selected[-1][1] + 1 if selected else offset,
            results=[
                dict(
                    chunk_id=r[0],
                    ordinal=r[1],
                    locator=r[2],
                    content=r[3],
                    content_truncated=bool(r[4]),
                    reference=reference(m, doc_id, r[0]),
                )
                for r in selected
            ],
        )


def seal_permissions(path, mode=0o444):
    try:
        os.chmod(path, mode)
        return True
    except OSError as error:
        if error.errno not in (errno.EPERM, errno.EINVAL, errno.ENOTSUP, errno.EOPNOTSUPP):
            raise
        return False  # FAT/exFAT may not expose POSIX permission bits.


def publish_no_replace(source, destination):
    # renameat2 also works on common removable filesystems without hard links.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = getattr(libc, "renameat2", None)
    if rename is not None:
        rename.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1) == 0:
            return
        error = ctypes.get_errno()
        if error not in (errno.ENOSYS, errno.EINVAL, errno.ENOTSUP):
            raise OSError(error, os.strerror(error), str(destination))
    try:
        os.link(source, destination)
        os.unlink(source)
        return
    except OSError as error:
        if error.errno not in (
            errno.EPERM,
            errno.ENOTSUP,
            errno.EOPNOTSUPP,
            errno.EXDEV,
            errno.ENOSYS,
        ):
            raise
    # Some removable filesystems support neither no-replace rename nor links.
    # Copy into a genuinely exclusive descriptor: never replace a placeholder,
    # which could have been swapped by a concurrent writer. During this fallback
    # the destination is visibly incomplete and must not be unplugged/imported.
    descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    owned = os.fstat(descriptor)
    try:
        with os.fdopen(descriptor, "wb") as target, open(source, "rb") as original:
            copied = 0
            heartbeat = time.monotonic()
            for block in iter(lambda: original.read(4 * 1024 * 1024), b""):
                target.write(block)
                copied += len(block)
                if time.monotonic() - heartbeat > 1:
                    emit(task="publish", phase="copy-removable", bytes=copied)
                    heartbeat = time.monotonic()
            target.flush()
            os.fsync(target.fileno())
            try:
                os.fchmod(target.fileno(), 0o444)
            except OSError:
                pass
        current = os.lstat(destination)
        if (current.st_dev, current.st_ino) != (owned.st_dev, owned.st_ino):
            raise FileExistsError("导出目标被其他程序替换，未覆盖该文件")
        os.unlink(source)
    except BaseException:
        try:
            current = os.lstat(destination)
            if (current.st_dev, current.st_ino) == (owned.st_dev, owned.st_ino):
                os.unlink(destination)
        except FileNotFoundError:
            pass
        raise


def export_library(source, destination, name, reserve_gib=2, max_gib=32):
    with contextlib.closing(open_readonly(source)) as check:
        sealed = manifest(check, source).get("format") != "legacy-sqlite"
    from smart_space_knowledge import build_lock

    # The same nonblocking lock excludes all cooperative builders/retry writers.
    with (contextlib.nullcontext() if sealed else build_lock(source)):
        return _export_locked(source, destination, name, reserve_gib, max_gib)


def _export_locked(source, destination, name, reserve_gib=2, max_gib=32):
    destination = Path(destination).expanduser().absolute()
    if destination.suffix.lower() != ".aikb":
        raise ValueError("Use the .aikb extension")
    if os.path.lexists(destination):
        raise FileExistsError("Destination already exists; choose another version filename")
    source = Path(source).resolve()
    storage_plan(
        destination.parent,
        source.stat().st_size + Path(str(source) + "-wal").stat().st_size
        if Path(str(source) + "-wal").exists()
        else source.stat().st_size,
        reserve_gib,
        max_gib,
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.parent / ("." + destination.name + ".partial-" + uuid.uuid4().hex)
    try:
        with contextlib.closing(open_readonly(source)) as src, contextlib.closing(
            sqlite3.connect(str(temporary))
        ) as dst:
            old = manifest(src, source)
            deadline = time.monotonic() + max(
                1, min(3600, float(os.environ.get("AI_SPACE_EXPORT_TIMEOUT_SECONDS", "1800")))
            )

            def progress(status, remaining, total):
                if time.monotonic() > deadline:
                    raise TimeoutError("导出达到时间上限，未发布不完整文件；请暂停写入任务后重试")
                guard_storage(temporary, reserve_gib, max_gib)
                emit(
                    task="export", phase="copy", completedPages=total - remaining, totalPages=total
                )

            src.backup(dst, pages=2048, progress=progress, sleep=0.01)
            dst.execute("PRAGMA journal_mode=DELETE")
            dst.execute("PRAGMA application_id=%d" % APPLICATION_ID)
            dst.execute("PRAGMA user_version=%d" % FORMAT_VERSION)
            dst.execute(
                "CREATE TABLE IF NOT EXISTS aikb_manifest(id INTEGER PRIMARY KEY CHECK(id=1),json TEXT NOT NULL)"
            )
            value = dict(
                format="aikb",
                format_version=1,
                library_id=old["library_id"],
                version=uuid.uuid4().hex[:16],
                name=name,
                created_at=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                originals=False,
                schema_version=2,
                documents=dst.execute("SELECT count(*) FROM documents").fetchone()[0],
                chunks=dst.execute("SELECT count(*) FROM chunks").fetchone()[0],
            )
            dst.execute(
                "INSERT OR REPLACE INTO aikb_manifest VALUES(1,?)",
                (json.dumps(value, ensure_ascii=False),),
            )
            dst.commit()
        guard_storage(temporary, reserve_gib, max_gib)
        with temporary.open("rb") as handle:
            os.fsync(handle.fileno())
        seal_permissions(temporary)
        publish_no_replace(temporary, destination)
        return dict(done=True, task="export", path=str(destination), manifest=value)
    finally:
        for suffix in ("", "-wal", "-shm", "-journal"):
            p = Path(str(temporary) + suffix)
            if p.exists():
                p.unlink()


def inspect_package(path):
    path = Path(path).resolve()
    if Path(str(path) + "-wal").exists() and Path(str(path) + "-wal").stat().st_size:
        raise ValueError("AIKB is not sealed; export a consistent snapshot first")
    with contextlib.closing(open_readonly(path)) as db:
        value = manifest(db, path, True)
        heartbeat = time.monotonic()
        emit(task="import", phase="verify", message="正在检查知识库完整性")

        def progress():
            nonlocal heartbeat
            if time.monotonic() - heartbeat >= 1:
                emit(task="import", phase="verify", message="正在检查知识库完整性")
                heartbeat = time.monotonic()
            return 0

        db.set_progress_handler(progress, 10000)
        if db.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise ValueError("Damaged library")
        counts = (
            db.execute("SELECT count(*) FROM documents").fetchone()[0],
            db.execute("SELECT count(*) FROM chunks").fetchone()[0],
        )
        if counts != (value["documents"], value["chunks"]):
            raise ValueError("Manifest counts do not match database")
        return value


def enable_version(library_id, version):
    selected = None
    with registry_write() as catalog:
        for row in catalog["libraries"]:
            if (row["library_id"], row["version"]) == (library_id, version):
                selected = row
        if selected is None:
            raise ValueError("Library version is not mounted")
        for row in catalog["libraries"]:
            if row["library_id"] == library_id:
                row["enabled"] = row["version"] == version
    return selected


def import_library(source, directory, mode="copy", reserve_gib=2, max_gib=32):
    source = Path(source).expanduser().resolve()
    initial = source.stat()
    if initial.st_size > max_gib * GIB:
        raise StorageLimitError(
            "Library exceeds configured size limit; increase the quota or select a smaller library"
        )
    value = inspect_package(source)
    # Same package mounted twice never copies it twice.
    for row in registry()["libraries"]:
        if (row["library_id"], row["version"]) == (value["library_id"], value["version"]):
            current = Path(row["path"])
            if current.is_file() and (current.stat().st_size, current.stat().st_mtime_ns) == (
                row["bytes"],
                row["mtime_ns"],
            ):
                if row.get("source_stamp") == [
                    str(source),
                    initial.st_ino,
                    initial.st_size,
                    initial.st_mtime_ns,
                ]:
                    return dict(
                        done=True,
                        task="import",
                        duplicate=True,
                        library=enable_version(row["library_id"], row["version"]),
                    )
                digest = hashlib.sha256()
                with source.open("rb") as stream:
                    while True:
                        block = stream.read(4 * 1024 * 1024)
                        if not block:
                            break
                        digest.update(block)
                if row.get("sha256") and digest.hexdigest() != row["sha256"]:
                    raise ValueError("Conflicting library identity/version")
                return dict(
                    done=True,
                    task="import",
                    duplicate=True,
                    library=enable_version(row["library_id"], row["version"]),
                )
    destination = Path(directory).expanduser().absolute() / (
        "%s-%s.aikb" % (value["library_id"], value["version"])
    )
    if mode == "copy" and not destination.exists():
        storage_plan(directory, source.stat().st_size, reserve_gib, max_gib)
    if source.stat().st_size > max_gib * GIB:
        raise StorageLimitError("Library exceeds configured size limit")
    digest = hashlib.sha256()
    temporary = None
    published = False
    try:
        if mode == "copy" and destination.exists():
            destination_before = destination.stat()
            if inspect_package(destination) != value:
                raise ValueError("Existing destination has different library metadata")

            def checksum(path):
                result = hashlib.sha256()
                with path.open("rb") as handle:
                    for block in iter(lambda: handle.read(4 * 1024 * 1024), b""):
                        result.update(block)
                return result.hexdigest()

            if checksum(source) != checksum(destination):
                raise ValueError("Existing destination content differs")
            source_after = source.stat()
            destination_after = destination.stat()
            if (initial.st_ino, initial.st_size, initial.st_mtime_ns) != (
                source_after.st_ino,
                source_after.st_size,
                source_after.st_mtime_ns,
            ) or (
                destination_before.st_ino,
                destination_before.st_size,
                destination_before.st_mtime_ns,
            ) != (
                destination_after.st_ino,
                destination_after.st_size,
                destination_after.st_mtime_ns,
            ):
                raise ValueError("Source or existing destination changed during import")
            source = destination
            mode = "mount"
            initial = destination_before
        if mode == "copy":
            destination.parent.mkdir(parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(prefix=".import-", dir=str(destination.parent))
            out = os.fdopen(fd, "wb")
        else:
            out = contextlib.nullcontext(None)
        before = initial
        with source.open("rb") as stream, out as target:
            copied = 0
            while True:
                chunk = stream.read(4 * 1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                copied += len(chunk)
                if target:
                    guard_storage(temporary, reserve_gib, max_gib)
                    target.write(chunk)
                if copied % (64 * 1024 * 1024) == 0:
                    emit(task="import", phase="verify", bytes=copied, totalBytes=before.st_size)
            if target:
                target.flush()
                os.fsync(target.fileno())
        after = source.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
        ):
            raise ValueError("Source changed during import")
        if mode == "copy":
            if inspect_package(temporary) != value:
                raise ValueError("Source manifest changed during import")
            seal_permissions(temporary, 0o400)
            publish_no_replace(temporary, destination)
            published = True
            active = destination
        else:
            active = source
        stat = active.stat()
        row = dict(
            value,
            path=str(active),
            sha256=digest.hexdigest(),
            bytes=stat.st_size,
            mtime_ns=stat.st_mtime_ns,
            read_only=True,
            source_stamp=[str(source), initial.st_ino, initial.st_size, initial.st_mtime_ns],
        )
        with registry_write() as catalog:
            existing = next(
                (
                    r
                    for r in catalog["libraries"]
                    if (r["library_id"], r["version"]) == (row["library_id"], row["version"])
                ),
                None,
            )
            if existing:
                if existing.get("sha256") != row["sha256"]:
                    raise ValueError("Conflicting library identity/version")
                if published:
                    destination.unlink()
                    published = False
                row = existing
            else:
                for previous in catalog["libraries"]:
                    if previous["library_id"] == row["library_id"]:
                        previous["enabled"] = False
                row["enabled"] = True
                catalog["libraries"].append(row)
        return dict(done=True, task="import", duplicate=False, library=row)
    except BaseException:
        if published and destination.exists():
            destination.unlink()
        raise
    finally:
        if temporary and os.path.exists(temporary):
            os.unlink(temporary)


def selected_paths(db=None, all_libraries=False, include_disabled=False):
    registry_error = None
    try:
        catalog = registry()
        known = [
            r
            for r in catalog["libraries"]
            if isinstance(r, dict) and (include_disabled or r.get("enabled", True))
        ]
        registry_error = "；".join(catalog.get("registry_warnings", [])) or None
    except (OSError, ValueError, TypeError) as exc:
        known = []
        registry_error = str(exc)

    def path_key(value):
        try:
            return str(Path(value).expanduser().resolve())
        except (OSError, ValueError, RuntimeError):
            return os.path.abspath(os.path.expanduser(value))

    personal = db or configured_db()
    rows = []
    try:
        if Path(personal).is_file():
            match = next((r for r in known if path_key(r["path"]) == path_key(personal)), None)
            rows.append(
                dict(match) if match else dict(path=personal, name="个人知识库", read_only=False)
            )
    except (OSError, ValueError, RuntimeError) as error:
        rows.append(
            dict(
                path=personal,
                name="个人知识库",
                available=False,
                availability_error="无法读取该库：" + str(error),
            )
        )
    if all_libraries:
        seen = {path_key(r["path"]) for r in rows}
        rows.extend(dict(r) for r in known if path_key(r["path"]) not in seen)
    if registry_error and all_libraries:
        rows.append(
            dict(
                path=str(registry_path()),
                name="库注册表",
                available=False,
                registry_error=registry_error,
            )
        )
    for row in rows:
        if not row.get("read_only"):
            continue
        try:
            path = Path(row["path"])
            row["available"] = path.is_file()
            if row["available"]:
                stat = path.stat()
                row["available"] = row.get("format") == "legacy-sqlite" or (
                    stat.st_size == row["bytes"] and stat.st_mtime_ns == row["mtime_ns"]
                )
        except (OSError, ValueError, RuntimeError) as error:
            row["available"] = False
            row["availability_error"] = "无法读取该库：" + str(error)
    return rows


def search_libraries(
    query, db=None, all_libraries=False, limit=20, budget_ms=2000, body_only=False
):
    started = time.monotonic()
    results = []
    coverage = []
    selected = selected_paths(db, all_libraries)
    for position, library in enumerate(selected):
        remaining = budget_ms - (time.monotonic() - started) * 1000
        if remaining <= 0 or library.get("available") is False:
            coverage.append(
                dict(
                    path=library["path"],
                    status="not_queried" if remaining <= 0 else "unavailable",
                    error=library.get("availability_error", library.get("registry_error", "")),
                )
            )
            continue
        try:
            answer = search_one(
                library["path"],
                query,
                limit,
                int(remaining / max(1, len(selected) - position)),
                body_only,
            )
        except (OSError, ValueError, sqlite3.Error) as error:
            coverage.append(dict(path=library["path"], status="error", error=str(error)))
            continue
        coverage.append(
            dict(
                path=library["path"],
                status="timed_out"
                if answer["timed_out"]
                else "unsupported_language"
                if answer.get("unsupported_language")
                else "queried",
                truncated=answer["truncated"],
                body_searched=answer["body_searched"],
            )
        )
        for rank, row in enumerate(answer["results"], 1):
            row["_fusion"] = 1 / (60 + rank)
            results.append(row)
    if not selected:
        coverage.append(
            dict(
                path=db or configured_db(),
                status="unavailable",
                error="No knowledge library is available",
            )
        )
    results.sort(key=lambda r: (r["lowConfidence"], -r["score"], -r["_fusion"]))
    for row in results:
        row.pop("_fusion")
    return dict(
        done=True,
        task="search",
        query=query,
        content_trust="untrusted_document",
        error="No library was queried successfully"
        if not any(r["status"] in ("queried", "unsupported_language") for r in coverage)
        else "",
        results=results[:limit],
        libraries=coverage,
        timed_out=any(r["status"] in ("timed_out", "not_queried") for r in coverage),
        incomplete=any(r["status"] != "queried" for r in coverage),
        truncated=len(results) > limit or any(r.get("truncated") for r in coverage),
        elapsed_ms=round((time.monotonic() - started) * 1000, 2),
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description="AI Space offline knowledge libraries (AIKB v1)")
    parser.add_argument("--db", help="Personal library; overrides AI Space configured path")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("libraries")
    commands.add_parser("repair-registry")
    statistics = commands.add_parser("stats")
    statistics.add_argument("--details", action="store_true")
    statistics.add_argument(
        "--fast", action="store_true", help="do not scan legacy OCR diagnostics"
    )
    p = commands.add_parser("enable")
    p.add_argument("--library-id", required=True)
    p.add_argument("--version", required=True)
    p = commands.add_parser("unmount")
    p.add_argument("--library-id", required=True)
    p.add_argument("--version", required=True)
    p = commands.add_parser("search")
    p.add_argument("--query", required=True)
    p.add_argument("--limit", type=int, default=20)
    p.add_argument("--budget-ms", type=int, default=2000)
    p.add_argument("--all", action="store_true")
    p.add_argument("--body-only", action="store_true")
    p.add_argument("--compact", action="store_true")
    p = commands.add_parser("fetch")
    p.add_argument("--reference")
    p.add_argument("--document-id", type=int)
    p.add_argument("--chunk-id", type=int)
    p.add_argument("--path")
    p.add_argument("--limit", type=int, default=8)
    p.add_argument("--offset", type=int)
    p.add_argument("--max-chars", type=int, default=32768)
    p = commands.add_parser("issues")
    p.add_argument("--limit", type=int, default=100)
    p = commands.add_parser("failures")
    p.add_argument("--ignored", action="store_true")
    p.add_argument("--offset", type=int, default=0)
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--category", choices=FAILURE_CATEGORIES, default="all")
    p = commands.add_parser("failure-policy")
    selection = p.add_mutually_exclusive_group(required=True)
    selection.add_argument("--document-ids-json")
    selection.add_argument("--all-matching", action="store_true")
    p.add_argument("--action", choices=["ignore", "restore"], required=True)
    p.add_argument("--category", choices=FAILURE_CATEGORIES, default="all")
    p.add_argument("--expected-selection", default="")
    p.add_argument("--expected-count", type=int, default=0)
    p = commands.add_parser("prune-absent")
    p.add_argument("--older-than-days", type=int, default=30)
    p.add_argument("--cutoff", default="")
    p.add_argument("--selection-token", default="")
    p.add_argument("--apply", action="store_true")
    p.add_argument("--reserve-gib", type=float, default=2)
    p.add_argument("--max-gib", type=float, default=32)
    for name in ("plan", "import", "export"):
        p = commands.add_parser(name)
        p.add_argument("--directory", required=name != "export")
        p.add_argument("--reserve-gib", type=float, default=2)
        p.add_argument("--max-gib", type=float, default=32)
        if name == "import":
            p.add_argument("--source", required=True)
            p.add_argument("--mode", choices=["copy", "mount"], default="copy")
        if name == "export":
            p.add_argument("--output", required=True)
            p.add_argument("--name", default="共享知识库")
    args = parser.parse_args(argv)

    def cancel(signum, frame):
        raise InterruptedError("Operation paused; unpublished temporary files removed")

    signal.signal(signal.SIGTERM, cancel)
    try:
        if args.command in ("import", "export", "prune-absent"):
            try:
                os.nice(15)
            except (OSError, AttributeError):
                pass
            if shutil.which("ionice"):
                import subprocess

                subprocess.run(
                    ["ionice", "-c", "3", "-p", str(os.getpid())],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
        db = args.db or configured_db()
        if args.command == "libraries":
            result = dict(
                done=True,
                task="libraries",
                libraries=selected_paths(args.db, True, True),
                registryWarnings=registry().get("registry_warnings", []),
            )
        elif args.command == "repair-registry":
            result = repair_registry()
        elif args.command == "prune-absent":
            from knowledge_maintenance import prune_absent

            result = prune_absent(
                db,
                args.older_than_days,
                args.cutoff,
                args.selection_token,
                args.apply,
                args.reserve_gib,
                args.max_gib,
            )
        elif args.command == "stats":
            result = stats(db, args.details, args.fast)
        elif args.command == "failures":
            result = failure_list(db, args.ignored, args.offset, args.limit, args.category)
        elif args.command == "failure-policy":
            result = (
                set_ignored_matching(
                    db,
                    args.category,
                    args.action == "ignore",
                    args.expected_selection,
                    args.expected_count,
                )
                if args.all_matching
                else set_ignored(db, document_ids(args.document_ids_json), args.action == "ignore")
            )
        elif args.command == "enable":
            result = dict(
                done=True, task="enable", library=enable_version(args.library_id, args.version)
            )
        elif args.command == "unmount":
            with registry_write() as catalog:
                before = len(catalog["libraries"])
                catalog["libraries"] = [
                    r
                    for r in catalog["libraries"]
                    if (r["library_id"], r["version"]) != (args.library_id, args.version)
                ]
                result = dict(
                    done=True,
                    task="unmount",
                    removed=before - len(catalog["libraries"]),
                    files_deleted=False,
                    warning="移除的版本若在使用中，该共享库将暂停检索；请手动启用保留版本。",
                )
        elif args.command == "search":
            result = search_libraries(
                args.query,
                args.db,
                args.all,
                max(1, min(args.limit, 200)),
                max(10, min(args.budget_ms, 30000)),
                args.body_only,
            )
            if args.compact:
                keys = {
                    "reference",
                    "name",
                    "locator",
                    "snippet",
                    "score",
                    "lowConfidence",
                    "extractionStatus",
                    "sourceAvailability",
                    "match_scope",
                    "hasBody",
                }
                result["results"] = [
                    {k: v for k, v in row.items() if k in keys} for row in result["results"]
                ]
        elif args.command == "fetch":
            if args.reference:
                match = re.fullmatch(
                    r"aispace://([a-f0-9-]+)/([A-Za-z0-9._-]+)/(\d+)/(\d+)", args.reference
                )
                if not match:
                    raise ValueError("Invalid knowledge reference")
                lib, version, doc, chunk = match.groups()
                found = False
                for item in selected_paths(args.db, True, True):
                    if item.get("available") is False:
                        continue
                    try:
                        with contextlib.closing(open_readonly(item["path"])) as conn:
                            m = manifest(conn, item["path"])
                            if m["library_id"] != lib:
                                continue
                            if m.get("format") == "legacy-sqlite" and version.startswith("doc-"):
                                row = conn.execute(
                                    "SELECT path,content_hash FROM documents WHERE id=?",
                                    (int(doc),),
                                ).fetchone()
                                if row:
                                    m = document_identity(m, row[0], row[1])
                            if m["version"] == version:
                                db = item["path"]
                                found = True
                                break
                    except (OSError, ValueError, sqlite3.Error):
                        continue
                if not found:
                    raise ValueError("Referenced library/version is not mounted")
                args.document_id, args.chunk_id = int(doc), int(chunk)
            if not args.reference and args.document_id is not None and not args.db:
                raise ValueError(
                    "Use --reference for cross-library fetch, or explicitly select --db before document-id"
                )
            result = fetch(
                db,
                args.document_id,
                args.chunk_id if args.offset is None else None,
                args.path,
                args.limit,
                args.offset or 0,
                max_chars=args.max_chars,
            )
        elif args.command == "issues":
            with contextlib.closing(open_readonly(db)) as conn:
                rows = conn.execute(
                    "SELECT d.path,e.status,e.issues_json FROM extraction_state e JOIN documents d ON d.id=e.document_id WHERE e.status NOT IN ('complete','no_text') LIMIT ?",
                    (max(1, min(args.limit, 1000)),),
                ).fetchall()
                result = dict(
                    done=True,
                    task="issues",
                    results=[
                        dict(
                            path=r[0],
                            status=r[1],
                            issues=json.loads(r[2]),
                            **issue_fields(json.loads(r[2])),
                        )
                        for r in rows
                    ],
                )
        elif args.command == "plan":
            result = dict(
                done=True,
                task="plan",
                **storage_plan(args.directory, 0, args.reserve_gib, args.max_gib),
            )
        elif args.command == "export":
            result = export_library(db, args.output, args.name, args.reserve_gib, args.max_gib)
        elif args.command == "import":
            result = import_library(
                args.source, args.directory, args.mode, args.reserve_gib, args.max_gib
            )
        emit(**result)
        return 1 if result.get("error") else 0
    except Exception as exc:
        emit(done=True, error=str(exc), task=args.command)
        return 1


if __name__ == "__main__":
    sys.exit(main())
