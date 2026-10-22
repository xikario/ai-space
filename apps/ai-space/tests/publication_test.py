"""Reject personal absolute home paths and private QA documents before release."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
# Include new files during local work, but never inspect ignored user data.
names = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=root).decode().split('\0')
personal_home = re.compile(r'/(?:home|Users)/[A-Za-z0-9_.-]+(?:/|\b)')
errors = []
for name in set(filter(None, names)):
    path = root / name
    if not path.is_file() or '/_vendor/' in name:
        continue
    if name.startswith('docs/') and (re.search(r'_\d{8}\.md$', name) or name == 'docs/VALIDATION.md'):
        errors.append(name + ': private dated acceptance record')
    if path.suffix not in {'.md', '.py', '.cpp', '.h', '.txt', '.yml', '.yaml', '.in'}:
        continue
    text = path.read_text(encoding='utf-8')
    if personal_home.search(text):
        errors.append(name + ': personal absolute home path')
    if name.startswith('docs/') and re.search(r'build/[^\s`]*(?:\.log|artifacts/)|releases/', text):
        errors.append(name + ': unavailable private acceptance artifact')
assert not errors, '\n'.join(sorted(errors))
print('PASS: public text excludes personal home paths and local acceptance records')
