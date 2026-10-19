#!/usr/bin/env python3
"""Local, structured, streaming extraction shared by previews and knowledge.

No UI character budget is applied here. Callers may stop iteration for a
preview; the knowledge worker consumes the stream to its end. Partial output
and unsupported content are explicit, never a claim of perfect fidelity.
"""
import codecs
import datetime
import io
import os
import posixpath
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections import deque
from pathlib import Path
from xml.etree import ElementTree as ET

VENDOR = Path(__file__).resolve().parent / "_vendor"
if VENDOR.is_dir() and str(VENDOR) not in sys.path:
    sys.path.insert(0, str(VENDOR))

EXTRACTOR_VERSION = "structured-1"
OLE_MAGIC = b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1"
TEXT_SUFFIXES = frozenset((
    ".txt .md .markdown .csv .tsv .json .xml .yaml .yml .ini .conf .log "
    ".html .htm .css .js .ts .cpp .c .h .hpp .py .sh .desktop .rtf").split())
OFFICE_SUFFIXES = frozenset(".doc .docx .wps .ppt .pptx .dps .xls .xlsx .et .odt .odp .ods".split())
IMAGE_SUFFIXES = frozenset(".jpg .jpeg .png .webp .tif .tiff .bmp .gif".split())
SUPPORTED_SUFFIXES = TEXT_SUFFIXES | OFFICE_SUFFIXES | IMAGE_SUFFIXES | {".pdf"}
LOG_SUMMARY_THRESHOLD = 1024 * 1024
NS_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"


def local(tag):
    return tag.rsplit("}", 1)[-1]


def children(element, name):
    return [child for child in element if local(child.tag) == name]


def attr(element, name, default=""):
    return next((v for k, v in element.attrib.items() if local(k) == name), default)


def clean(value):
    return re.sub(r"[ \t]+", " ", value.replace("\x00", " ")).strip()


def natural(value):
    return [int(p) if p.isdigit() else p for p in re.split(r"(\d+)", value)]


def run_to_file(arguments, output, timeout=120):
    with open(output, "wb") as handle:
        result = subprocess.run(arguments, stdout=handle, stderr=subprocess.PIPE,
                                timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError("%s failed (%s)" % (Path(arguments[0]).name, result.returncode))


def detect_kind(path):
    """OLE takes precedence over an embedded/appended ZIP directory."""
    with path.open("rb") as stream:
        magic = stream.read(8)
    if magic == OLE_MAGIC:
        import olefile
        with olefile.OleFileIO(str(path)) as ole:
            if ole.exists("Workbook") or ole.exists("Book"):
                return "xls"
            if ole.exists("WordDocument"):
                return "doc"
            if ole.exists("PowerPoint Document"):
                return "ppt-binary"
        return "ole-unknown"
    if magic.startswith(b"PK") or zipfile.is_zipfile(str(path)):
        with zipfile.ZipFile(str(path)) as archive:
            names = set(archive.namelist())
            if "word/document.xml" in names:
                return "docx"
            if "xl/workbook.xml" in names or "xl/sharedStrings.xml" in names:
                return "xlsx"
            if "ppt/presentation.xml" in names or any(
                    re.fullmatch(r"ppt/slides/slide\d+\.xml", n) for n in names):
                return "pptx"
            if "content.xml" in names:
                return "odf"
        return "zip-unknown"
    if magic.startswith(b"%PDF-"):
        return "pdf"
    if path.suffix.lower() in TEXT_SUFFIXES:
        return "text"
    if path.suffix.lower() in IMAGE_SUFFIXES:
        return "image"
    # A misleading OOXML extension must not result in an empty successful ZIP
    # extraction. Explicit failure is retained in the knowledge database.
    return "unknown"


class Extraction:
    def __init__(self, path, ocr=False, progress=None):
        self.path = Path(path)
        self.ocr = ocr
        self.progress = progress
        self.status = "complete"
        self.issues = []
        self.kind = "unknown"
        self.blocks = 0

    def partial(self, reason):
        if self.status != "failed":
            self.status = "partial"
        if reason not in self.issues:
            self.issues.append(reason)

    def __iter__(self):
        self.kind = detect_kind(self.path)
        handler = getattr(self, "extract_" + self.kind.replace("-", "_"), None)
        if handler is None:
            raise ValueError("unsupported container: " + self.kind)
        for locator, text in handler():
            text = text.replace("\x00", " ").strip()
            if text:
                self.blocks += 1
                yield locator, text
                if self.progress and self.blocks % 100 == 0:
                    self.progress(self.blocks)

    def extract_docx(self):
        with zipfile.ZipFile(str(self.path)) as archive:
            styles = {}
            if "word/styles.xml" in archive.namelist():
                root = ET.fromstring(archive.read("word/styles.xml"))
                for style in root:
                    sid = attr(style, "styleId")
                    for el in style.iter():
                        if local(el.tag) == "outlineLvl":
                            styles[sid] = int(attr(el, "val", "0")) + 1
            headings = []
            paragraph = table = 0
            def emit_body(element):
                nonlocal headings, paragraph, table
                tag = local(element.tag)
                if tag == "p":
                    paragraph += 1
                    value = word_text(element)
                    level = 0
                    for el in element.iter():
                        if local(el.tag) == "outlineLvl":
                            level = int(attr(el, "val", "0")) + 1
                        elif local(el.tag) == "pStyle" and not level:
                            sid = attr(el, "val")
                            match = re.search(r"(?:heading|标题)\s*(\d)", sid, re.I)
                            level = styles.get(sid, int(match.group(1)) if match else 0)
                    if value:
                        if level:
                            headings = headings[:max(0, level - 1)] + [value]
                        loc = "段落 %d" % paragraph
                        if headings:
                            loc = "章节: %s | %s" % (" > ".join(headings), loc)
                        yield loc, value
                elif tag == "tbl":
                    table += 1
                    prefix = "章节: %s | " % " > ".join(headings) if headings else ""
                    for row_no, value in word_table(element):
                        yield prefix + "表格 %d 行%d" % (table, row_no), value
                elif tag in {"sdt", "sdtContent", "customXml"}:
                    for child in element:
                        if local(child.tag) not in {"sdtPr", "sdtEndPr", "customXmlPr"}:
                            yield from emit_body(child)
                elif tag != "sectPr":
                    value = word_text(element)
                    if value:
                        self.partial("nonstandard_body_element:" + tag)
                        yield "正文扩展", value
            # Only body children are released: nested table text remains intact
            # until the table is emitted, while large documents do not accumulate.
            with archive.open("word/document.xml") as stream:
                stack = []
                for event, element in ET.iterparse(stream, events=("start", "end")):
                    if event == "start":
                        stack.append(element)
                        continue
                    parent = stack[-2] if len(stack) > 1 else None
                    if parent is not None and local(parent.tag) == "body":
                        yield from emit_body(element)
                        parent.remove(element)
                    stack.pop()

    def extract_xlsx(self):
        with zipfile.ZipFile(str(self.path)) as archive:
            # The string pool is kept on disk: a large enterprise directory must
            # not produce a second full in-memory copy during cell reconstruction.
            import sqlite3
            with tempfile.TemporaryDirectory(prefix="smart-space-xlsx-") as tmp:
                pool = sqlite3.connect(str(Path(tmp) / "strings.sqlite"))
                try:
                    pool.execute("CREATE TABLE s(id INTEGER PRIMARY KEY, value TEXT)")
                    if "xl/sharedStrings.xml" in archive.namelist():
                        with archive.open("xl/sharedStrings.xml") as stream:
                            idx = 0
                            root = None
                            for event, element in ET.iterparse(stream, events=("start", "end")):
                                if root is None:
                                    root = element
                                if event == "end" and local(element.tag) == "si":
                                    value = "".join(el.text or "" for el in element.iter()
                                                    if local(el.tag) == "t")
                                    pool.execute("INSERT INTO s VALUES(?,?)", (idx, value))
                                    idx += 1
                                    root.remove(element)
                        pool.commit()
                    sheets, epoch = workbook_sheets(archive)
                    date_styles = workbook_date_styles(archive)
                    if not sheets:
                        self.partial("workbook_has_no_worksheets")
                        for (value,) in pool.execute("SELECT value FROM s ORDER BY id"):
                            yield "共享字符串（工作表缺失）", value
                    for sheet_name, member in sheets:
                        headers = {}
                        row_fallback = 0
                        with archive.open(member) as stream:
                            sheet_data = None
                            stack = []
                            for event, element in ET.iterparse(stream, events=("start", "end")):
                                if event == "start":
                                    stack.append(element)
                                    if local(element.tag) == "sheetData" and len(stack) == 2:
                                        sheet_data = element
                                    continue
                                parent = stack[-2] if len(stack) > 1 else None
                                stack.pop()
                                # Extension metadata may also have elements named
                                # row; only direct worksheet rows are cell data.
                                if local(element.tag) != "row" or parent is not sheet_data:
                                    continue
                                row_fallback += 1
                                row_no = element.get("r", str(row_fallback))
                                values = []
                                for index, cell in enumerate(children(element, "c"), 1):
                                    ref = cell.get("r", column_name(index) + row_no)
                                    col = re.sub(r"\d", "", ref)
                                    try:
                                        value = xlsx_cell(cell, pool, epoch, date_styles)
                                    except (ValueError, OverflowError) as error:
                                        self.partial("cell_format_unresolved:%s!%s:%s" % (sheet_name, ref, error))
                                        raw = children(cell, "v")
                                        value = (raw[0].text or "") if raw else ""
                                        if value:
                                            value += "（原始单元格值，格式解析失败）"
                                    if value != "":
                                        values.append((col, ref, value))
                                # Keep cell coordinates even when the initial row
                                # is a title instead of a header. Do not guess rows.
                                if not headers and len(values) >= 2:
                                    headers = {col: value[:160] for col, _ref, value in values}
                                    is_header = True
                                else:
                                    is_header = False
                                if values:
                                    parts = []
                                    for col, ref, value in values:
                                        label = headers.get(col, "") if not is_header else ""
                                        parts.append("%s%s: %s" %
                                                     (ref, " (%s)" % label if label else "", value))
                                    yield "工作表:%s 行%s" % (sheet_name, row_no), " | ".join(parts)
                                if sheet_data is not None:
                                    sheet_data.remove(element)
                                else:
                                    element.clear()
                finally:
                    pool.close()

    def extract_pptx(self):
        with zipfile.ZipFile(str(self.path)) as archive:
            names = set(archive.namelist())
            order = []
            if "ppt/presentation.xml" in names and "ppt/_rels/presentation.xml.rels" in names:
                rels = relationships(archive, "ppt/presentation.xml")
                root = ET.fromstring(archive.read("ppt/presentation.xml"))
                for group in children(root, "sldIdLst"):
                    for el in children(group, "sldId"):
                        rid = el.get("{%s}id" % NS_REL, "")
                        if rid in rels:
                            order.append(rels[rid])
                        else:
                            self.partial("missing_slide_relationship:" + rid)
            else:
                self.partial("slide_relationships_missing:natural_order_fallback")
                order = sorted((n for n in names if re.fullmatch(
                    r"ppt/slides/slide\d+\.xml", n)), key=natural)
            for number, member in enumerate(order, 1):
                if member not in names:
                    self.partial("missing_slide:" + member)
                    continue
                root = ET.fromstring(archive.read(member))
                trees = [el for el in root.iter() if local(el.tag) == "spTree"]
                title = ""
                for el in root.iter():
                    if local(el.tag) == "sp" and any(local(ph.tag) == "ph" and
                            ph.get("type") in {"title", "ctrTitle"} for ph in el.iter()):
                        title = drawing_text(el)
                        break
                locator = "第 %d 页%s" % (number, " | " + title[:160] if title else "")
                if not trees:
                    value = drawing_text(root)
                    if value:
                        yield locator, value
                    continue
                for shape in trees[0]:
                    if any(local(el.tag) == "ph" and el.get("type") in
                           {"sldNum", "dt", "ftr", "hdr"} for el in shape.iter()):
                        continue
                    tables = [el for el in shape.iter() if local(el.tag) == "tbl"]
                    if tables:
                        for table_no, table in enumerate(tables, 1):
                            for row_no, row in enumerate(children(table, "tr"), 1):
                                value = " | ".join("列%d: %s" % (i, drawing_text(cell))
                                                   for i, cell in enumerate(children(row, "tc"), 1))
                                yield locator + " 表格%d 行%d" % (table_no, row_no), value
                    else:
                        value = drawing_text(shape)
                        if value:
                            yield locator, value

    def extract_xls(self):
        import xlrd
        workbook = xlrd.open_workbook(str(self.path), on_demand=True)
        try:
            for name in workbook.sheet_names():
                sheet = workbook.sheet_by_name(name)
                headers = {}
                for row_no in range(sheet.nrows):
                    values = []
                    for col_no, cell in enumerate(sheet.row(row_no), 1):
                        kind, value = cell.ctype, cell.value
                        if kind in (xlrd.XL_CELL_EMPTY, xlrd.XL_CELL_BLANK):
                            continue
                        if kind == xlrd.XL_CELL_DATE:
                            value = xlrd.xldate_as_datetime(value, workbook.datemode).isoformat(sep=" ")
                        elif kind == xlrd.XL_CELL_BOOLEAN:
                            value = "TRUE" if value else "FALSE"
                        elif kind == xlrd.XL_CELL_ERROR:
                            value = xlrd.error_text_from_code.get(value, "#ERROR")
                        elif isinstance(value, float) and value.is_integer():
                            value = int(value)
                        value = str(value).strip()
                        if value:
                            values.append((col_no, value))
                    is_header = not headers and len(values) >= 2
                    if is_header:
                        headers = {col: value[:160] for col, value in values}
                    parts = []
                    for col, value in values:
                        label = headers.get(col, "") if not is_header else ""
                        parts.append("%s%d%s: %s" % (column_name(col), row_no + 1,
                            " (%s)" % label if label else "", value))
                    if parts:
                        yield "工作表:%s 行%d" % (name, row_no + 1), " | ".join(parts)
                if hasattr(workbook, "unload_sheet"):
                    workbook.unload_sheet(name)
        finally:
            workbook.release_resources()

    def extract_doc(self):
        # Recover only the Word main story via its piece table. Reading arbitrary
        # binary strings would index embedded images, font tables and stale text.
        import olefile
        self.partial("legacy_word:main_text_only;layout_not_preserved")
        with olefile.OleFileIO(str(self.path)) as ole:
            word = ole.openstream("WordDocument").read()
            if len(word) < 154:
                raise ValueError("truncated Word FIB")
            flags = struct.unpack_from("<H", word, 10)[0]
            if flags & 0x0100:
                raise ValueError("encrypted Word document")
            pos = 32
            csw = struct.unpack_from("<H", word, pos)[0]
            pos += 2 + 2 * csw
            cslw = struct.unpack_from("<H", word, pos)[0]
            lw = pos + 2
            ccp_text = struct.unpack_from("<I", word, lw + 12)[0]
            pos = lw + 4 * cslw
            pairs = struct.unpack_from("<H", word, pos)[0]
            pos += 2
            if pairs <= 33:
                raise ValueError("unsupported Word FIB version")
            fc, size = struct.unpack_from("<II", word, pos + 33 * 8)
            table_name = "1Table" if flags & 0x0200 else "0Table"
            table = ole.openstream(table_name)
            table.seek(fc)
            clx = table.read(size)
            at = 0
            while at < len(clx) and clx[at] == 1:
                at += 3 + struct.unpack_from("<H", clx, at + 1)[0]
            if at >= len(clx) or clx[at] != 2:
                raise ValueError("Word piece table missing")
            length = struct.unpack_from("<I", clx, at + 1)[0]
            plc = clx[at + 5:at + 5 + length]
            pieces = (length - 4) // 12
            if len(plc) != length or pieces < 1 or (length - 4) % 12:
                raise ValueError("invalid Word piece table")
            buffer = ""
            paragraph = 0
            for i in range(pieces):
                cp, end = struct.unpack_from("<II", plc, i * 4)
                count = min(end, ccp_text) - cp
                if count <= 0:
                    break
                offset = struct.unpack_from("<I", plc, 4 * (pieces + 1) + i * 8 + 2)[0]
                compressed = bool(offset & 0x40000000)
                offset &= 0x3fffffff
                if compressed:
                    offset //= 2
                raw = word[offset:offset + count * (1 if compressed else 2)]
                if len(raw) != count * (1 if compressed else 2):
                    raise ValueError("truncated Word text piece")
                value = raw.decode("cp1252" if compressed else "utf-16le", errors="replace")
                # Remove field instructions, retaining visible field results.
                buffer += value
                while "\r" in buffer or len(buffer) > 8192:
                    split = buffer.find("\r")
                    if split < 0 or split > 8192:
                        split = 8192
                    skip = 1 if buffer[split:split + 1] == "\r" else 0
                    line, buffer = buffer[:split], buffer[split + skip:]
                    line = re.sub("\x13[^\x14\x15]*(?:\x14)?", "", line).replace("\x15", "")
                    line = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", " ", line)
                    if line.strip():
                        paragraph += 1
                        yield "旧版Word 正文段落 %d" % paragraph, line
            if buffer.strip():
                buffer = re.sub("\x13[^\x14\x15]*(?:\x14)?", "", buffer).replace("\x15", "")
                yield "旧版Word 正文段落 %d" % (paragraph + 1), buffer

    def extract_pdf(self):
        pdftotext = shutil.which("pdftotext")
        if not pdftotext:
            raise RuntimeError("pdftotext not installed")
        with tempfile.TemporaryDirectory(prefix="smart-space-pdf-") as tmp:
            target = Path(tmp) / "text.txt"
            result = subprocess.run([pdftotext, "-layout", "-enc", "UTF-8", str(self.path), str(target)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                    timeout=180, check=False)
            if result.returncode:
                raise RuntimeError("pdftotext failed (%d)" % result.returncode)
            page = 0
            with target.open(encoding="utf-8") as stream:
                buffer = ""
                while True:
                    value = stream.read(65536)
                    buffer += value
                    while "\f" in buffer:
                        text, buffer = buffer.split("\f", 1)
                        page += 1
                        yield from self.pdf_page(text, page, tmp)
                    if not value:
                        if buffer.strip():
                            page += 1
                            yield from self.pdf_page(buffer, page, tmp)
                        break

    def pdf_page(self, text, page, tmp):
        if text.strip():
            yield "第 %d 页" % page, text
        elif self.ocr:
            value = self.ocr_page(page, tmp)
            if value.strip():
                yield "第 %d 页（OCR）" % page, value
            else:
                self.partial("empty_ocr_page:%d" % page)
        else:
            self.partial("page_needs_ocr:%d" % page)
        if self.progress:
            self.progress(page)

    def ocr_page(self, page, tmp):
        if not shutil.which("pdftoppm") or not shutil.which("tesseract"):
            raise RuntimeError("pdftoppm/tesseract not installed")
        prefix = Path(tmp) / "ocr-page"
        image = Path(str(prefix) + ".png")
        value = Path(tmp) / "ocr.txt"
        try:
            result = subprocess.run(["pdftoppm", "-f", str(page), "-l", str(page),
                "-singlefile", "-r", "150", "-png", str(self.path), str(prefix)],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=180, check=False)
            if result.returncode or not image.exists():
                raise RuntimeError("PDF page rendering failed: %d" % page)
            run_to_file(["tesseract", str(image), "stdout", "-l", "chi_sim+eng"], str(value), 120)
            return value.read_text(encoding="utf-8")
        finally:
            if image.exists():
                image.unlink()

    def extract_image(self):
        if not self.ocr:
            self.partial("image_needs_ocr")
            return
        if not shutil.which("tesseract"):
            raise RuntimeError("tesseract not installed")
        with tempfile.TemporaryDirectory(prefix="smart-space-image-") as tmp:
            text = Path(tmp) / "ocr.txt"
            run_to_file(["tesseract", str(self.path), "stdout", "-l", "chi_sim+eng"], str(text), 120)
            yield "图片全文（OCR）", text.read_text(encoding="utf-8")

    def extract_text(self):
        with self.path.open("rb") as raw:
            prefix = raw.read(65536)
        if prefix.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)):
            encoding = "utf-16"
        else:
            try:
                codecs.getincrementaldecoder("utf-8-sig")().decode(prefix, final=False)
                encoding = "utf-8-sig"
            except UnicodeError:
                encoding = "gb18030"
        summary = self.path.suffix.lower() == ".log" and self.path.stat().st_size > LOG_SUMMARY_THRESHOLD
        if summary:
            self.partial("large_log_summary:head_errors_context_tail")
        tail = deque(maxlen=100)
        context = deque(maxlen=3)
        following = 0
        emitted_until = 0
        with self.path.open(encoding=encoding, errors="strict") as stream:
            for no, line in enumerate(stream, 1):
                if not summary:
                    yield "行 %d" % no, line
                    continue
                tail.append((no, line))
                error = bool(re.search(r"error|fail|warn|exception", line, re.I))
                if error:
                    for prev_no, prev_line in context:
                        if prev_no > emitted_until:
                            yield "日志行 %d（筛选摘要）" % prev_no, prev_line
                            emitted_until = prev_no
                    following = 3
                if no <= 100 or error or following:
                    if no > emitted_until:
                        yield "日志行 %d（筛选摘要）" % no, line
                        emitted_until = no
                    following = max(0, following - 1)
                context.append((no, line))
        for no, line in tail:
            if no > emitted_until:
                yield "日志行 %d（尾部摘要）" % no, line

    def extract_odf(self):
        # Preserve paragraph/table boundaries for existing ODF support. More
        # advanced page/layout mapping requires an office renderer.
        self.partial("odf:paragraph_locations_only")
        with zipfile.ZipFile(str(self.path)) as archive:
            with archive.open("content.xml") as stream:
                no = 0
                for event, el in ET.iterparse(stream, events=("end",)):
                    if local(el.tag) in {"p", "h"}:
                        value = "".join(el.itertext())
                        if value.strip():
                            no += 1
                            yield "ODF 段落 %d" % no, value
                        el.clear()


def word_text(element):
    def walk(el):
        if local(el.tag) in {"instrText", "del", "delText"}:
            return ""
        if local(el.tag) == "r" and any(local(v.tag) == "vanish" and
                attr(v, "val", "true").lower() not in {"0", "false", "off"} for v in el.iter()):
            return ""
        if local(el.tag) == "t":
            return el.text or ""
        if local(el.tag) == "tab":
            return "\t"
        if local(el.tag) in {"br", "cr"}:
            return "\n"
        return "".join(walk(child) for child in el)
    return walk(element).strip()


def word_table(table):
    headers = {}
    for row_no, row in enumerate(children(table, "tr"), 1):
        values = []
        row_headers = {}
        col = 1
        before = next((attr(el, "val", "0") for el in row.iter() if local(el.tag) == "gridBefore"), "0")
        col += int(before)
        for cell in children(row, "tc"):
            text = " / ".join(word_text(p) for p in cell if local(p.tag) == "p" and word_text(p))
            for nested in children(cell, "tbl"):
                text += " / " + " / ".join("嵌套表格行%d %s" % (n, v) for n, v in word_table(nested))
            span = next((int(attr(el, "val", "1")) for el in cell.iter()
                         if local(el.tag) == "gridSpan"), 1)
            merge = next((attr(el, "val", "continue") for el in cell.iter()
                          if local(el.tag) == "vMerge"), "")
            label = "列%d" % col
            if span > 1:
                label += "-%d" % (col + span - 1)
            if col in headers and headers[col]:
                label += " (%s)" % headers[col][:160]
            if merge:
                label += "（纵向合并:%s）" % merge
            values.append((label, text))
            for c in range(col, col + span):
                row_headers[c] = text
            col += span
        if row_no == 1:
            headers = row_headers
        if values:
            yield row_no, " | ".join("%s: %s" % pair for pair in values)


def drawing_text(element):
    paragraphs = [el for el in element.iter() if local(el.tag) == "p"]
    if paragraphs:
        return "\n".join("".join(el.text or "" for el in p.iter() if local(el.tag) == "t")
                         for p in paragraphs).strip()
    return " ".join(el.text or "" for el in element.iter() if local(el.tag) == "t").strip()


def relationships(archive, member):
    directory, name = posixpath.split(member)
    rel_path = posixpath.join(directory, "_rels", name + ".rels")
    root = ET.fromstring(archive.read(rel_path))
    return {el.get("Id"): posixpath.normpath(posixpath.join(directory, el.get("Target", "")))
            if not el.get("Target", "").startswith("/") else el.get("Target").lstrip("/")
            for el in root if el.get("TargetMode") != "External"}


def workbook_sheets(archive):
    names = set(archive.namelist())
    if "xl/workbook.xml" not in names:
        return [(Path(n).stem, n) for n in sorted(names, key=natural)
                if re.fullmatch(r"xl/worksheets/sheet\d+\.xml", n)], False
    root = ET.fromstring(archive.read("xl/workbook.xml"))
    date1904 = any(local(el.tag) == "workbookPr" and el.get("date1904") in {"1", "true"}
                   for el in root.iter())
    rels = relationships(archive, "xl/workbook.xml")
    result = []
    for el in root.iter():
        if local(el.tag) == "sheet":
            rid = el.get("{%s}id" % NS_REL, "")
            if rid not in rels or rels[rid] not in names:
                raise ValueError("missing worksheet relationship: " + rid)
            result.append((el.get("name", "工作表"), rels[rid]))
    return result, date1904


def workbook_date_styles(archive):
    if "xl/styles.xml" not in archive.namelist():
        return set()
    root = ET.fromstring(archive.read("xl/styles.xml"))
    custom = {int(el.get("numFmtId")): el.get("formatCode", "")
              for el in root.iter() if local(el.tag) == "numFmt"}
    styles = set()
    for group in children(root, "cellXfs"):
        for i, el in enumerate(group):
            n = int(el.get("numFmtId", "0"))
            pattern = re.sub(r'"[^\"]*"|\\.|\[[^\]]*\]', "", custom.get(n, ""))
            if n in set(range(14, 23)) | set(range(27, 37)) | set(range(45, 48)) | set(range(50, 59)) or re.search(r"[ydhs]", pattern, re.I):
                styles.add(i)
    return styles


def column_name(number):
    result = ""
    while number:
        number, remainder = divmod(number - 1, 26)
        result = chr(65 + remainder) + result
    return result


def xlsx_cell(cell, pool, date1904, date_styles):
    kind = cell.get("t", "n")
    values = children(cell, "v")
    value = values[0].text or "" if values else ""
    if kind == "s":
        if not value:
            return ""
        row = pool.execute("SELECT value FROM s WHERE id=?", (int(value),)).fetchone()
        if row is None:
            raise ValueError("shared string index out of bounds: " + value)
        return row[0]
    if kind == "inlineStr":
        return "".join(el.text or "" for el in cell.iter() if local(el.tag) == "t")
    if kind == "b":
        return "TRUE" if value == "1" else "FALSE"
    formulas = children(cell, "f")
    if not value and formulas:
        return "=%s（未提供缓存值）" % (formulas[0].text or "")
    if value and kind == "n" and int(cell.get("s", "0")) in date_styles:
        number = float(value)
        epoch = datetime.datetime(1904, 1, 1) if date1904 else datetime.datetime(1899, 12, 30)
        if not date1904 and 0 < number < 60:
            number += 1
        return (epoch + datetime.timedelta(days=number)).isoformat(sep=" ")
    return value
