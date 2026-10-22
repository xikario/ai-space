"""Real minimal OLE presentations, current-edit selection and safe retry."""
import contextlib
import json
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1]/'scripts'
sys.path.insert(0, str(SCRIPTS))
import legacy_ppt
from smart_space_extract import Extraction, OLE_MAGIC


def record(kind, payload=b'', container=False, instance=0):
    return struct.pack('<HHI', instance*16+(15 if container else 0), kind, len(payload))+payload


def text(value, wide=True):
    return record(0x0FA0 if wide else 0x0FA8, value.encode('utf-16le' if wide else 'latin1'))


def slide(value=None, payload=None):
    body = text(value) if payload is None else payload
    for kind in (0xF00D, 0xF004, 0xF003, 0xF002, 0x040C, 0x03EE):
        body = record(kind, body, True)
    return body


def document(slides, other=b''):
    body = b''
    for ident, outline in slides:
        body += record(0x03F3, struct.pack('<IIIII', ident, 0, len(outline), 256+ident, 0))
        for value in outline:
            body += record(0x0F9F, struct.pack('<I', 4))+text(value)
    return record(0x03E8, other+record(0x0FF0, body, True), True)


def current_user(offset, encrypted=False):
    return record(0x0FF6, struct.pack('<IIIHHBBH', 20,
        0xF3D1C4DF if encrypted else 0xE391C05F, offset, 0, 0x03F4, 3, 0, 0)+struct.pack('<I', 8))


class PresentationFixture:
    def __init__(self):self.data = bytearray();self.current = 0
    def add(self, value):
        offset = len(self.data);self.data.extend(value);return offset
    def edit(self, objects, previous=0, crypto=None):
        directory = b''.join(struct.pack('<II', 0x100000+ident, offset) for ident,offset in objects.items())
        at = self.add(record(0x1772, directory))
        payload = struct.pack('<IIIIIIHH', 256, 0x03000000, previous, at, 1, max(objects), 1, 0)
        if crypto is not None:payload += struct.pack('<I', crypto)
        self.current = self.add(record(0x0FF5, payload));return self.current
    def write(self, path, encrypted=False):
        write_ole(path, bytes(self.data), current_user(self.current, encrypted))


def write_ole(path, presentation, user):
    """Two regular streams, avoiding mini-FAT to keep fixtures inspectable."""
    streams = [presentation, user]
    padded = [value.ljust(max(4096, (len(value)+511)//512*512), b'\0') for value in streams]
    sizes = [len(value)//512 for value in padded]
    starts = [1, 1+sizes[0]];fat_sector = 1+sum(sizes)
    if fat_sector >= 128:raise ValueError('fixture needs more than one FAT sector')
    end, free = 0xFFFFFFFE, 0xFFFFFFFF
    header = bytearray(512);header[:8] = OLE_MAGIC
    struct.pack_into('<HHHH', header, 24, 0x003E, 3, 0xFFFE, 9)
    struct.pack_into('<H', header, 32, 6)
    struct.pack_into('<IIIIIIIII', header, 40, 0, 1, 0, 0, 4096, end, 0, end, 0)
    struct.pack_into('<109I', header, 76, fat_sector, *([free]*108))
    directory = bytearray(512)
    for index, (name, kind, start, size) in enumerate([
            ('Root Entry', 5, end, 0),
            ('PowerPoint Document', 2, starts[0], len(padded[0])),
            ('Current User', 2, starts[1], len(padded[1]))]):
        pos = index*128;encoded = (name+'\0').encode('utf-16le')
        directory[pos:pos+len(encoded)] = encoded
        struct.pack_into('<HBBIII', directory, pos+64, len(encoded), kind, 1,
            2 if index==1 else free, free, 1 if index==0 else free)
        struct.pack_into('<IQ', directory, pos+116, start, size)
    fat = [free]*128;fat[0] = end;fat[fat_sector] = 0xFFFFFFFD
    for start, count in zip(starts, sizes):
        for sector in range(start, start+count):fat[sector] = sector+1 if sector+1<start+count else end
    path.write_bytes(bytes(header)+bytes(directory)+b''.join(padded)+struct.pack('<128I', *fat))


def single_slide(path, value='中文正文\r下一段'):
    fixture = PresentationFixture()
    objects = {1:fixture.add(document([(3, [])])), 3:fixture.add(slide(value))}
    fixture.edit(objects);fixture.write(path);return fixture


class LegacyPptTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory();self.root = Path(self.temp.name)
    def tearDown(self):self.temp.cleanup()

    def test_actual_ole_ppt_and_dps_read_unicode_bytes_and_page_locators(self):
        for suffix in ('.ppt', '.dps', '.docx'):
            path = self.root/('中文名'+suffix)
            fixture = PresentationFixture()
            objects = {1:fixture.add(document([(3, []), (4, [])])),
                3:fixture.add(slide('中文正文\r第二段')),
                4:fixture.add(slide(payload=text('Café\vnext', False)))}
            fixture.edit(objects);fixture.write(path)
            before = path.read_bytes();result = Extraction(path);blocks = list(result)
            self.assertEqual(result.kind, 'ppt-binary');self.assertEqual(result.status, 'partial')
            self.assertEqual(blocks, [('第 1 页（旧版演示）', '中文正文\n第二段'), ('第 2 页（旧版演示）', 'Café\nnext')])
            self.assertIn('legacy_ppt:slide_text_only', result.issues);self.assertEqual(before, path.read_bytes())

    def test_current_edit_order_excludes_superseded_removed_master_and_notes(self):
        path = self.root/'history.ppt';fixture = PresentationFixture()
        other = b''.join(record(0x0FF0, record(0x03F3, struct.pack('<IIIII', ident,0,0,256,0)), True, instance) for instance,ident in ((1,2),(2,6)))
        old = {1:fixture.add(document([(3,[]),(4,[]),(5,[])], other)),
            2:fixture.add(slide('MASTER_SECRET')), 3:fixture.add(slide('OBSOLETE_SECRET')),
            4:fixture.add(slide('保留的页面')), 5:fixture.add(slide('DELETED_SECRET')),
            6:fixture.add(slide('NOTES_SECRET'))}
        previous = fixture.edit(old)
        current = {1:fixture.add(document([(4,[]),(3,[])], other)),
            3:fixture.add(slide('最新正文')), 7:fixture.add(slide('ORPHAN_SECRET'))}
        fixture.edit(current, previous);fixture.write(path)
        blocks = list(Extraction(path));self.assertEqual([t for _,t in blocks], ['保留的页面','最新正文'])

    def test_outline_references_resolve_once_in_drawing_order_and_unknown_atoms_skip(self):
        path = self.root/'outline.ppt';fixture = PresentationFixture()
        outline = ['占位标题','占位正文','未关联但仍属于当前页的正文']
        body = record(0x0F9E, struct.pack('<I',1))+record(0x0F9E, struct.pack('<I',0))+text('图形中的附加文字')
        body += record(0x7777, text('BINARY_NOISE'), True)
        objects = {1:fixture.add(document([(3,outline)])),3:fixture.add(slide(payload=body))}
        fixture.edit(objects);fixture.write(path)
        self.assertEqual(list(Extraction(path))[0][1], '占位正文\n占位标题\n图形中的附加文字\n未关联但仍属于当前页的正文')

    def test_encrypted_current_user_and_optional_encryption_reference_rejected(self):
        path = self.root/'encrypted.ppt';fixture = single_slide(path)
        fixture.write(path, encrypted=True)
        with self.assertRaisesRegex(ValueError,'已加密'):list(Extraction(path))
        fixture.edit({1:0}, crypto=9);fixture.write(path)
        with self.assertRaisesRegex(ValueError,'已加密'):list(Extraction(path))

    def test_malformed_bounds_and_invalid_outline_fail_instead_of_indexing_noise(self):
        path = self.root/'damaged.ppt';fixture = single_slide(path)
        struct.pack_into('<I', fixture.data, 4, 0xFFFFFFFF);fixture.write(path)
        with self.assertRaisesRegex(ValueError,'边界'):list(Extraction(path))
        fixture = PresentationFixture()
        fixture.edit({1:fixture.add(document([(3,[])])),3:fixture.add(slide(payload=record(0x0F9E, struct.pack('<I',10))))});fixture.write(path)
        with self.assertRaisesRegex(ValueError,'超出范围'):list(Extraction(path))

    def test_cycles_missing_streams_and_dangling_current_objects_rejected(self):
        path = self.root/'cycle.ppt';fixture = single_slide(path)
        struct.pack_into('<I',fixture.data,fixture.current+16,fixture.current);fixture.write(path)
        with self.assertRaisesRegex(ValueError,'引用无效'):list(Extraction(path))
        ole = mock.Mock();ole.exists.return_value = False
        with self.assertRaisesRegex(ValueError,'必要的数据流'):legacy_ppt.LegacyPresentation(ole)
        fixture = PresentationFixture();fixture.edit({1:fixture.add(document([(99,[])]))});fixture.write(path)
        with self.assertRaisesRegex(ValueError,'对象引用'):list(Extraction(path))

    def test_resource_limits_and_no_readable_text_are_explicit(self):
        path = self.root/'limited.ppt';single_slide(path)
        for name, limit in [('MAX_STREAM_BYTES',1),('MAX_RECORDS',1),('MAX_DEPTH',1),('MAX_SECONDS',0)]:
            with self.subTest(limit=name),mock.patch.object(legacy_ppt,name,limit):
                with self.assertRaisesRegex(ValueError,'限额'):list(Extraction(path))
        single_slide(path,'')
        with self.assertRaisesRegex(ValueError,'未找到可提取的文字'):list(Extraction(path))

    def test_worker_build_search_retry_failure_and_other_documents_preserved(self):
        path = self.root/'real.ppt';single_slide(path,'首版知识正文');other = self.root/'other.txt';other.write_text('其他文档必须保留')
        db = self.root/'knowledge.sqlite';index = self.root/'index.json'
        index.write_text(json.dumps(dict(items=[dict(path=str(p),name=p.name,root=str(self.root),isDir=False) for p in (path,other)])))
        def build(*extra):
            out = subprocess.check_output([sys.executable,str(SCRIPTS/'smart_space_knowledge.py'),'build','--db',str(db),'--index-json',str(index),'--no-ocr',*extra],text=True)
            return json.loads(out.splitlines()[-1])
        self.assertEqual(build()['updated'],2);self.assertEqual(build()['reused'],2)
        # The PPT revision must not make cached TXT/PDF/DOCX files stale.
        from smart_space_extract import parser_version, EXTRACTOR_VERSION
        self.assertEqual(parser_version(other),EXTRACTOR_VERSION+"+safe-text-2")
        self.assertIn(legacy_ppt.PARSER_VERSION,parser_version(path))
        with contextlib.closing(sqlite3.connect(db)) as conn:
            old_other = conn.execute('SELECT content FROM chunks WHERE document_id=2').fetchall()
        single_slide(path,'重试后最新知识结论')
        self.assertEqual(build('--force','--retry-document-id','1')['updated'],1)
        import knowledge_library as lib
        self.assertTrue(lib.search_one(db,'最新知识结论')['results'])
        before = lib.fetch(db,1)['results'];single_slide(path,'')
        build('--force','--retry-document-id','1')
        self.assertEqual(before,lib.fetch(db,1)['results'])
        self.assertEqual(lib.failure_list(db)['results'][0]['status'],'failed')
        with contextlib.closing(sqlite3.connect(db)) as conn:
            self.assertEqual(old_other,conn.execute('SELECT content FROM chunks WHERE document_id=2').fetchall())
            self.assertEqual(conn.execute('PRAGMA integrity_check').fetchone()[0],'ok')

    def test_new_partial_and_page_diagnostics_are_chinese(self):
        import knowledge_library as lib
        fields=lib.issue_fields(['legacy_ppt:slide_text_only','legacy_ppt_page_without_text:2'])
        self.assertIn('已提取旧版演示文稿',fields['reason']);self.assertIn('第 2 页未提取到文字',fields['reason'])
        self.assertNotIn('legacy_ppt',fields['reason'])


if __name__ == '__main__':unittest.main()
