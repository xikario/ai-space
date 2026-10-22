#!/usr/bin/env python3
"""Read-only bridge to the installed AI Space knowledge worker."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

# Keep historical Agent command lines working while using one query core.
argv=sys.argv[1:]
legacy=('--status' in argv or '--fetch' in argv or ('--json' in argv and '--query' not in argv))
if legacy:
    old=argparse.ArgumentParser(description='Legacy AI Space read-only query')
    old.add_argument('words',nargs='*');old.add_argument('--db');old.add_argument('--limit',type=int,default=15)
    old.add_argument('--budget-ms',type=int,default=2000);old.add_argument('--status',action='store_true');old.add_argument('--fetch')
    old.add_argument('--mode');old.add_argument('--json',action='store_true');old.add_argument('--body-only',action='store_true')
    known=old.parse_args(argv);prefix=['--db',known.db] if known.db else []
    if known.status:argv=prefix+['stats']
    elif known.fetch:argv=prefix+['fetch','--reference' if known.fetch.startswith('aispace://') else '--path',known.fetch,'--limit',str(known.limit)]
    else:argv=prefix+['search','--all','--query',' '.join(known.words),'--limit',str(known.limit),'--budget-ms',str(known.budget_ms)]+(['--body-only'] if known.body_only else [])

else:argv=[value for value in argv if value!='--json']

parser = argparse.ArgumentParser()
parser.add_argument('--db', help='Explicit library; otherwise discover AI Space storage settings')
parser.add_argument('command', choices=['stats', 'search', 'issues', 'fetch', 'libraries'])
args, rest = parser.parse_known_args(argv)
if '--db' in rest or any(v.startswith('--db=') for v in rest):
    parser.error('place --db before the command')
source = Path(__file__).resolve().parents[3]/'scripts/knowledge_library.py'
candidates = [Path(__file__).with_name('knowledge_library.py'), source]
binary = shutil.which('ai-space')
if binary:
    candidates.append(Path(binary).resolve().parent.parent/'libexec/ai-space/knowledge_library.py')
candidates += [Path.home()/'.local/libexec/ai-space/knowledge_library.py', Path('/usr/local/libexec/ai-space/knowledge_library.py'), Path('/usr/libexec/ai-space/knowledge_library.py')]
worker = next((p for p in candidates if p.is_file()), None)
if worker is None:
    parser.error('install AI Space or run from its source checkout')
sys.exit(subprocess.call([sys.executable, str(worker), *(['--db', args.db] if args.db else []), args.command, *rest]))
