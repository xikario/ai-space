#!/usr/bin/env python3
"""Read-only bridge to the installed AI Space knowledge worker."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--db', default=str(Path(os.environ.get('XDG_DATA_HOME', str(Path.home()/'.local/share')))/'ai-space/knowledge/smart-space-knowledge.sqlite'))
parser.add_argument('command', choices=['stats', 'search', 'issues'])
args, rest = parser.parse_known_args()
if '--db' in rest or any(v.startswith('--db=') for v in rest):
    parser.error('place --db before the command')
source = Path(__file__).resolve().parents[3]/'scripts/smart_space_knowledge.py'
candidates = [source]
binary = shutil.which('ai-space')
if binary:
    candidates.append(Path(binary).resolve().parent.parent/'libexec/ai-space/smart_space_knowledge.py')
candidates += [Path.home()/'.local/libexec/ai-space/smart_space_knowledge.py', Path('/usr/local/libexec/ai-space/smart_space_knowledge.py'), Path('/usr/libexec/ai-space/smart_space_knowledge.py')]
worker = next((p for p in candidates if p.is_file()), None)
if worker is None:
    parser.error('install AI Space or run from its source checkout')
sys.exit(subprocess.call([sys.executable, str(worker), args.command, '--db', args.db, *rest]))
