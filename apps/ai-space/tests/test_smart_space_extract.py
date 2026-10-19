import importlib.util
import io
import json
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import smart_space_extract as extract


def write_zip(path, members):
    with zipfile.ZipFile(path, "w") as archive:
        for name, value in members.items():
            archive.writestr(name, value)


class StructuredExtractionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_xlsx_restores_cell_references_headers_dates_and_sheet_order(self):
        path = self.root / "products.xlsx"
        write_zip(path, {
            "xl/workbook.xml": '<workbook xmlns:r="%s"><sheets><sheet name="产品" r:id="r2"/></sheets></workbook>' % extract.NS_REL,
            "xl/_rels/workbook.xml.rels": '<Relationships><Relationship Id="r2" Target="worksheets/sheet9.xml"/></Relationships>',
            "xl/sharedStrings.xml": '<sst><si><t>产品</t></si><si><t>CPU核数</t></si><si><t>工业版</t></si></sst>',
            "xl/styles.xml": '<styleSheet><cellXfs><xf numFmtId="0"/><xf numFmtId="14"/></cellXfs></styleSheet>',
            "xl/worksheets/sheet9.xml": '<worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="C1" t="s"><v>1</v></c></row><row r="12"><c r="A12" t="s"><v>2</v></c><c r="C12"><v>16</v></c><c r="D12" s="1"><v>45292</v></c><c r="E12"><f>SUM(C2:C11)</f></c></row></sheetData></worksheet>',
        })
        result = extract.Extraction(path)
        blocks = list(result)
        self.assertEqual(result.status, "complete")
        self.assertEqual(blocks[1][0], "工作表:产品 行12")
        self.assertIn("A12 (产品): 工业版", blocks[1][1])
        self.assertIn("C12 (CPU核数): 16", blocks[1][1])
        self.assertIn("2024-01-01", blocks[1][1])
        self.assertIn("未提供缓存值", blocks[1][1])

    def test_pptx_uses_presentation_relationship_order(self):
        path = self.root / "slides.pptx"
        members = {
            "ppt/presentation.xml": '<p:presentation xmlns:p="p" xmlns:r="%s"><p:sldIdLst><p:sldId r:id="r7"/><p:sldId r:id="r2"/><p:sldId r:id="r10"/></p:sldIdLst></p:presentation>' % extract.NS_REL,
            "ppt/_rels/presentation.xml.rels": '<Relationships>' + ''.join(
                '<Relationship Id="r%d" Target="slides/slide%d.xml"/>' % (n,n) for n in (2,7,10)) + '</Relationships>'}
        for n in (2,7,10):
            members['ppt/slides/slide%d.xml' % n] = '<p:sld xmlns:p="p" xmlns:a="a"><p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>SLIDE_%d</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>' % n
        write_zip(path, members)
        blocks = list(extract.Extraction(path))
        self.assertEqual([text for _loc,text in blocks], ['SLIDE_7','SLIDE_2','SLIDE_10'])
        self.assertEqual([loc for loc,_text in blocks], ['第 1 页','第 2 页','第 3 页'])

    def test_xlsx_extension_rows_empty_strings_and_invalid_date_preserve_document(self):
        path=self.root/'edge-cells.xlsx'
        write_zip(path,{
            'xl/sharedStrings.xml':'<sst><si><t>实际正文</t></si></sst>',
            'xl/styles.xml':'<styleSheet><cellXfs><xf numFmtId="0"/><xf numFmtId="14"/></cellXfs></styleSheet>',
            'xl/worksheets/sheet1.xml':'<worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"/></row><row r="2"><c r="A2" s="1"><v>1234567890</v></c></row></sheetData><extLst><ext><row><c r="A999"><v>EXTENSION_METADATA</v></c></row></ext></extLst></worksheet>'})
        result=extract.Extraction(path)
        blocks=list(result)
        text='\n'.join(t for _,t in blocks)
        self.assertIn('实际正文',text)
        self.assertIn('1234567890',text)
        self.assertNotIn('EXTENSION_METADATA',text)
        self.assertEqual(result.status,'partial')
        self.assertTrue(any('cell_format_unresolved' in issue for issue in result.issues))

    def test_docx_preserves_flow_tables_and_filters_hidden_field_instructions(self):
        path = self.root / "report.docx"
        write_zip(path, {"word/document.xml": '''<w:document xmlns:w="w"><w:body>
          <w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>部署要求</w:t></w:r></w:p>
          <w:p><w:r><w:instrText>PAGE MERGEFORMAT</w:instrText></w:r><w:r><w:rPr><w:vanish/></w:rPr><w:t>SECRET_HIDDEN</w:t></w:r><w:r><w:rPr><w:vanish w:val="0"/></w:rPr><w:t>VISIBLE_TEXT</w:t></w:r><w:r><w:t>前言</w:t></w:r></w:p>
          <w:tbl><w:tr><w:tc><w:p><w:r><w:t>版本</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>核数</w:t></w:r></w:p></w:tc></w:tr><w:tr><w:tc><w:p><w:r><w:t>工业版</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>16</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
          <w:p><w:r><w:t>结论</w:t></w:r></w:p></w:body></w:document>'''})
        blocks = list(extract.Extraction(path))
        text = '\n'.join(t for _loc,t in blocks)
        self.assertNotIn('MERGEFORMAT', text)
        self.assertNotIn('SECRET_HIDDEN', text)
        self.assertIn('VISIBLE_TEXT', text)
        self.assertIn('列2 (核数): 16', text)
        self.assertEqual(blocks[-1][1], '结论')
        self.assertIn('章节: 部署要求', blocks[2][0])

    def test_ole_header_wins_over_embedded_zip(self):
        path = self.root / 'book.xls'
        path.write_bytes(extract.OLE_MAGIC + b'padding')
        with zipfile.ZipFile(path, 'a') as archive:
            archive.writestr('theme/theme1.xml', '<theme/>')
        self.assertTrue(zipfile.is_zipfile(path))
        ole = mock.MagicMock()
        ole.__enter__.return_value.exists.side_effect = lambda name: name == 'Workbook'
        with mock.patch('olefile.OleFileIO', return_value=ole):
            self.assertEqual(extract.detect_kind(path), 'xls')

    def test_legacy_word_piece_table_avoids_binary_noise(self):
        path = self.root / 'fake.docx'
        path.write_bytes(extract.OLE_MAGIC)
        text = '正文第一段\r中文结论'
        word = bytearray(2048)
        struct.pack_into('<H', word, 32, 14)
        struct.pack_into('<H', word, 62, 22)
        struct.pack_into('<I', word, 76, len(text))
        struct.pack_into('<H', word, 152, 34)
        plc = struct.pack('<II',0,len(text)) + b'\0\0' + struct.pack('<I',1024) + b'\0\0'
        clx = b'\2' + struct.pack('<I',len(plc)) + plc
        struct.pack_into('<II',word,154+33*8,0,len(clx))
        word[1024:1024+len(text.encode('utf-16le'))] = text.encode('utf-16le')
        ole = mock.MagicMock()
        ole.__enter__.return_value.openstream.side_effect = lambda name: io.BytesIO(bytes(word) if name == 'WordDocument' else clx)
        ole.__enter__.return_value.exists.side_effect = lambda name: name == 'WordDocument'
        with mock.patch('olefile.OleFileIO', return_value=ole):
            result = extract.Extraction(path)
            blocks = list(result)
        self.assertEqual(result.kind, 'doc')
        self.assertEqual(result.status, 'partial')
        self.assertIn('中文结论', '\n'.join(v for _,v in blocks))

    def test_pdf_ocr_has_no_five_page_or_character_quota(self):
        result = extract.Extraction(self.root / 'scan.pdf', ocr=True)
        with mock.patch.object(result,'ocr_page',side_effect=lambda page,tmp: '第%d页正文' % page + '字'*4000):
            blocks = [block for page in range(1,9) for block in result.pdf_page('',page,str(self.root))]
        self.assertEqual(len(blocks),8)
        self.assertIn('第8页正文',blocks[-1][1])
        self.assertEqual(sum(len(t) for _,t in blocks),32040)

    def test_large_logs_are_explicit_summaries_with_error_context_and_tail(self):
        path = self.root / 'journal.log'
        lines = ['noise %d\n' % i for i in range(1000)]
        lines[500] = 'ERROR_MARKER\n'
        lines[-1] = 'TAIL_MARKER\n'
        path.write_text(''.join(lines))
        result = extract.Extraction(path)
        with mock.patch.object(extract,'LOG_SUMMARY_THRESHOLD',10):
            blocks = list(result)
        text = '\n'.join(t for _,t in blocks)
        self.assertIn('ERROR_MARKER',text)
        self.assertIn('noise 499',text)
        self.assertIn('TAIL_MARKER',text)
        self.assertNotIn('noise 300\n',text)
        self.assertEqual(result.status,'partial')


class DeepKnowledgeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.db = self.root / 'knowledge.sqlite'
        self.snapshot = self.root / 'index.json'

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self, paths):
        self.snapshot.write_text(json.dumps({'items':[{'path':str(p),'name':p.name,'root':str(self.root),
            'category':'document','content':'','size':p.stat().st_size if p.exists() else 0} for p in paths]}))

    def build(self,*args):
        out = subprocess.check_output([sys.executable,str(ROOT/'scripts/smart_space_knowledge.py'),
             'build','--index-json',str(self.snapshot),'--db',str(self.db),'--no-ocr',*args],text=True)
        return json.loads(out.splitlines()[-1])

    def test_full_body_survives_empty_ui_preview_and_reuses_only_matching_policy(self):
        path = self.root / 'long.txt'
        path.write_text('前段材料。'*6000+'唯一尾部结论MARKER')
        self.manifest([path])
        self.build()
        with sqlite3.connect(self.db) as con:
            self.assertTrue(con.execute("SELECT 1 FROM chunks WHERE content LIKE '%唯一尾部结论MARKER%'").fetchone())
            self.assertEqual(con.execute('SELECT status FROM extraction_state').fetchone()[0],'complete')
        self.assertEqual(self.build()['reused'],1)
        self.assertEqual(self.build('--chunk-size','600')['updated'],1)

    def test_failed_reextract_and_offline_source_preserve_last_good_chunks(self):
        path = self.root / 'report.docx'
        write_zip(path, {'word/document.xml':'<w:document xmlns:w="w"><w:body><w:p><w:r><w:t>已验证结论</w:t></w:r></w:p></w:body></w:document>'})
        self.manifest([path]); self.build()
        path.write_bytes(b'corrupt file')
        self.build()
        with sqlite3.connect(self.db) as con:
            self.assertEqual(con.execute('SELECT status FROM extraction_state').fetchone()[0],'failed')
            self.assertIn('已验证结论',con.execute('SELECT content FROM chunks').fetchone()[0])
        path.unlink(); self.build()
        with sqlite3.connect(self.db) as con:
            self.assertEqual(con.execute('SELECT count(*) FROM documents').fetchone()[0],1)
            self.assertEqual(con.execute('SELECT status FROM extraction_state').fetchone()[0],'missing')
            self.assertEqual(con.execute('PRAGMA integrity_check').fetchone()[0],'ok')

    def test_scoped_refresh_cannot_delete_other_roots(self):
        a=self.root/'a'; b=self.root/'b'; a.mkdir();b.mkdir()
        p=a/'a.txt';q=b/'b.txt';p.write_text('范围甲');q.write_text('范围乙')
        self.manifest([p,q]);self.build()
        self.manifest([p]);self.build('--include-root',str(a))
        with sqlite3.connect(self.db) as con:
            self.assertEqual(con.execute('SELECT count(*) FROM documents').fetchone()[0],2)

    def test_parser_upgrade_reextracts_and_failed_document_remains_visible(self):
        path=self.root/'text.txt'; path.write_text('正确正文')
        self.manifest([path]);self.build()
        with sqlite3.connect(self.db) as con:
            con.execute("UPDATE extraction_state SET source_signature='old-parser'")
        self.assertEqual(self.build()['updated'],1)
        bad=self.root/'bad.docx';bad.write_bytes(b'not an office container')
        self.manifest([path,bad]);self.build()
        with sqlite3.connect(self.db) as con:
            self.assertEqual(con.execute('SELECT count(*) FROM documents').fetchone()[0],2)
            self.assertEqual(con.execute("SELECT status FROM extraction_state e JOIN documents d ON d.id=e.document_id WHERE name='bad.docx'").fetchone()[0],'failed')

    def test_rebuild_cleans_derived_fts_orphans(self):
        path=self.root/'text.txt';path.write_text('正确正文')
        self.manifest([path]);self.build()
        with sqlite3.connect(self.db) as con:
            con.execute("INSERT INTO chunks_fts(name,path,content,locator,document_id,chunk_id) VALUES('ghost','','ghost','',99999,99999)")
        self.build()
        with sqlite3.connect(self.db) as con:
            self.assertEqual(con.execute('SELECT count(*) FROM chunks_fts f LEFT JOIN chunks c ON c.id=f.chunk_id WHERE c.id IS NULL').fetchone()[0],0)

    def test_legacy_fts_rowids_are_migrated_without_losing_raw_text(self):
        path=self.root/'text.txt';path.write_text('行号迁移验收正文')
        self.manifest([path]);self.build()
        with sqlite3.connect(self.db) as con:
            con.execute('UPDATE chunks_fts SET rowid=rowid+1000')
            con.execute("DELETE FROM meta WHERE key='fts_rowid_layout'")
        self.build()
        with sqlite3.connect(self.db) as con:
            self.assertEqual(con.execute('SELECT count(*) FROM chunks_fts WHERE rowid!=chunk_id').fetchone()[0],0)
            self.assertIn('行号迁移验收正文',con.execute('SELECT content FROM chunks').fetchone()[0])


if __name__ == '__main__':
    unittest.main()
