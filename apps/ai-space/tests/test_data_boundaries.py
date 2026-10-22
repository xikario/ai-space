"""Synthetic hostile/large-file cases; never use the user's actual documents."""
import codecs
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from smart_space_extract import Extraction
import smart_space_indexer as indexer
from text_safety import open_regular_binary
import xml_safety


class DataBoundaries(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ai-space-boundaries-')
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def extract(self, name, content, **kwargs):
        path = self.root / name
        path.write_bytes(content)
        result = Extraction(path, **kwargs)
        return result, list(result)

    def test_transport_stream_rejected_but_typescript_kept(self):
        packet = b'G' + b'\xff' * 187
        with self.assertRaisesRegex(ValueError, 'binary_content'):
            self.extract('video.ts', packet * 10)
        result, blocks = self.extract('source.ts', 'const 项目 = "正常代码";'.encode())
        self.assertIn('正常代码', blocks[0][1])

    def test_binary_with_text_suffix_is_not_body(self):
        with self.assertRaisesRegex(ValueError, 'binary_content'):
            self.extract('data.txt', b'\x7fELF\0\x01\x02' * 100)

    def test_unicode_bom_is_not_binary(self):
        for encoding in ('utf-16', 'utf-32', 'utf-8-sig'):
            result, blocks = self.extract(encoding + '.txt', '中文资料与检索'.encode(encoding))
            self.assertIn('中文资料', ''.join(text for _, text in blocks))

    def test_document_char_cap_retains_partial_text(self):
        with mock.patch.dict(os.environ, AI_SPACE_MAX_DOCUMENT_CHARS='2048'):
            result, blocks = self.extract('large.txt', b'A' * (2 * 1024 * 1024))
        self.assertEqual(sum(len(text) for _, text in blocks), 2048)
        self.assertEqual(result.status, 'partial')
        self.assertIn('document_text_limit:2048', result.issues)

    def test_late_bad_byte_does_not_erase_prior_text(self):
        result, blocks = self.extract('mixed.txt', b'valid text\n' * 7000 + b'bad byte: \xff\nend of file')
        self.assertIn('end of file', ''.join(text for _, text in blocks))
        self.assertEqual(result.status, 'partial')
        self.assertIn('decode_errors_replaced', result.issues)

    def test_preview_never_uses_read_bytes_on_whole_file(self):
        path = self.root / 'large.txt'
        path.write_bytes('中文预览'.encode('utf-16'))
        with mock.patch.object(Path, 'read_bytes', side_effect=AssertionError('unbounded read')):
            self.assertIn('中文预览', indexer.extract_plain_text(path, 1024))

    def test_fifo_and_symlink_do_not_block(self):
        fifo = self.root / 'pipe.txt'
        os.mkfifo(fifo)
        link = self.root / 'alias.txt'
        link.symlink_to(fifo)
        for path in (fifo, link):
            with self.assertRaises(ValueError):
                with open_regular_binary(path):
                    self.fail('special file accepted')
            self.assertEqual(indexer.extract_plain_text(path, 100), '')

    def test_build_named_directories_are_scanned(self):
        for name in ('build', 'build-clean', 'build-v10'):
            folder = self.root / name
            folder.mkdir()
            (folder / '资料.txt').write_text('建筑资料', encoding='utf-8')
        output = self.root / 'index.json'
        subprocess.run([sys.executable, str(SCRIPTS / 'smart_space_indexer.py'),
                        '--roots-json', json.dumps([str(self.root)]), '--output', str(output),
                        '--fast-full', '--metadata-only'], check=True, stdout=subprocess.DEVNULL, timeout=15)
        value = json.loads(output.read_text())
        self.assertEqual(sum(item['name'] == '资料.txt' for item in value['items']), 3)

    def test_odf_nested_bold_and_link_text_survives(self):
        path = self.root / 'rich.odt'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('content.xml', '<doc xmlns:text="urn:text"><text:p>前文<text:span>加粗文字</text:span><text:a>链接文字</text:a>结尾</text:p></doc>')
        self.assertIn('前文加粗文字链接文字结尾', ''.join(text for _, text in Extraction(path)))

    def test_rtf_controls_and_hidden_destinations_are_not_body(self):
        result, blocks = self.extract('rich.rtf', br'{\rtf1\ansi{\fonttbl HiddenFont;}Visible\par {\b bold} \u20013?\u25991?{\pict hiddenPicture}}')
        text = ''.join(value for _, value in blocks)
        self.assertIn('Visible\nbold 中文', text)
        self.assertNotIn('HiddenFont', text)
        self.assertNotIn('hiddenPicture', text)
        self.assertNotIn('\\par', text)
        self.assertEqual(result.status, 'partial')

    def test_rtf_multibyte_legacy_chinese(self):
        _, blocks = self.extract('gb.rtf', br"{\rtf1\ansi\ansicpg936 \'d6\'d0\'ce\'c4}")
        self.assertEqual(''.join(value for _, value in blocks), '中文')

    def test_large_xlsx_non_cell_metadata_stays_bounded(self):
        import tracemalloc
        path = self.root / 'metadata.xlsx'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('xl/workbook.xml', '<workbook xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets></workbook>')
            archive.writestr('xl/_rels/workbook.xml.rels', '<Relationships><Relationship Id="rId1" Target="worksheets/sheet1.xml"/></Relationships>')
            archive.writestr('xl/worksheets/sheet1.xml', '<worksheet><cols>' + '<col min="1" max="2" width="10"/>' * 60000 + '</cols><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>正文保留</t></is></c></row></sheetData></worksheet>')
        tracemalloc.start()
        try:
            text = ''.join(value for _, value in Extraction(path))
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        self.assertIn('正文保留', text)
        self.assertLess(peak, 12 * 1024 * 1024)

    def test_dtd_rejected_even_across_read_boundaries(self):
        with self.assertRaisesRegex(ValueError, 'xml_dtd'):
            xml_safety.fromstring(b'<!DOCTYPE x [<!ENTITY a "text">]><x>&a;</x>')
        import io
        stream = xml_safety.CheckedStream(io.BytesIO(b'<!DOCTYPE x><x/>'))
        stream.read(5)
        with self.assertRaisesRegex(ValueError, 'xml_dtd'):
            stream.read(30)


if __name__ == '__main__':
    unittest.main()
