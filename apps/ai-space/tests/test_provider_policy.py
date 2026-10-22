import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from provider_policy import audit, load_policy, verify_command
from smart_space_indexer import collect_provider_items, run_provider


class ProviderPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='provider-policy-')
        self.root = Path(self.temporary.name)
        self.program = self.root / 'provider'
        self.program.write_text('#!/bin/sh\nprintf "[]"\n', encoding='utf-8')
        self.program.chmod(0o700)
        self.config = self.root / 'providers.json'
        self.write_config(str(self.program))

    def tearDown(self):
        self.temporary.cleanup()

    def write_config(self, program, **extra):
        row = dict(type='command', program=program, arguments=[], **extra)
        self.config.write_text(json.dumps(dict(providers=[row]), ensure_ascii=False), encoding='utf-8')

    def test_unchanged_command_uses_resolved_path(self):
        rows, digest = load_policy(self.config)
        self.assertEqual(rows[0]['program'], str(self.program.resolve()))
        self.assertEqual(audit(self.config)['digest'], digest)
        self.assertEqual(run_provider(rows[0], []), [])

    def test_same_path_replacement_invalidates_approval(self):
        rows, approved = load_policy(self.config)
        before = self.program.stat()
        replacement = self.root / 'replacement'
        replacement.write_text('#!/bin/sh\nprintf "{}"\n', encoding='utf-8')
        replacement.chmod(0o700)
        os.utime(replacement, ns=(before.st_atime_ns, before.st_mtime_ns))
        os.replace(replacement, self.program)
        self.assertNotEqual(load_policy(self.config)[1], approved)
        with self.assertRaisesRegex(ValueError, 'changed'):
            verify_command(rows[0])
        with mock.patch.dict(os.environ, AI_SPACE_PROVIDER_APPROVAL=approved):
            errors = []
            self.assertEqual(collect_provider_items(self.config, [], errors), [])
        self.assertIn('approval', errors[0]['error'])

    def test_content_change_even_with_same_inode_size_and_mtime(self):
        _, before = load_policy(self.config)
        info = self.program.stat()
        self.program.write_text('#!/bin/sh\nprintf "{}"\n', encoding='utf-8')
        os.utime(self.program, ns=(info.st_atime_ns, info.st_mtime_ns))
        self.assertEqual(self.program.stat().st_size, info.st_size)
        self.assertNotEqual(load_policy(self.config)[1], before)

    def test_path_resolution_change_requires_new_approval(self):
        second = self.root / 'other'
        second.mkdir()
        other = second / 'provider'
        other.write_bytes(self.program.read_bytes())
        other.chmod(0o700)
        self.write_config('provider')
        with mock.patch.dict(os.environ, PATH=str(self.root)):
            first = load_policy(self.config)
        with mock.patch.dict(os.environ, PATH=str(second)):
            last = load_policy(self.config)
            self.assertEqual(verify_command(first[0][0]), str(self.program))
        self.assertNotEqual(first[1], last[1])

    def test_utf8_configuration_ignores_locale(self):
        self.write_config(str(self.program), name='中文数据源')
        with mock.patch.dict(os.environ, LC_ALL='C'):
            self.assertEqual(load_policy(self.config)[0][0]['name'], '中文数据源')

    def test_oversized_configuration_rejected(self):
        self.config.write_bytes(b' ' * (1024 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, '1 MiB'):
            load_policy(self.config)

    def test_unresolvable_program_rejected_before_execution(self):
        self.write_config(str(self.root / 'missing'))
        with self.assertRaisesRegex(ValueError, 'executable'):
            load_policy(self.config)


if __name__ == '__main__':
    unittest.main()
