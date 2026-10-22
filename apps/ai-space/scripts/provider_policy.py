#!/usr/bin/env python3
"""Read and describe a local Provider capability set before authorizing it."""
import hashlib
import json
import os
import shutil
from pathlib import Path
import sys

MAX_CONFIG=1024*1024

def load_policy(path):
    path=Path(path).expanduser().resolve();base=path.parent
    def read(source):
        if source.stat().st_size>MAX_CONFIG:raise ValueError('Provider configuration exceeds 1 MiB')
        value=json.loads(source.read_text())
        if not isinstance(value,dict):raise ValueError('Provider configuration must be an object')
        return value
    config=read(path);rows=config.get('providers',[])
    if not isinstance(rows,list):raise ValueError('providers must be an array')
    active=[]
    for row in rows:
        if not isinstance(row,dict):raise ValueError('Invalid provider entry')
        if not row.get('enabled',True):continue
        row=dict(row);inherited=row.pop('inheritFrom',None);key=row.pop('inheritKey','')
        if inherited:
            source=Path(os.path.expandvars(inherited)).expanduser();source=(base/source).resolve() if not source.is_absolute() else source.resolve()
            if source!=base and base not in source.parents:raise ValueError('inheritFrom must stay inside the configuration directory')
            value=read(source)
            for part in str(key).split('.') if key else []:value=value.get(part) if isinstance(value,dict) else None
            if not isinstance(value,dict):raise ValueError('inheritKey must select an object')
            if value.get('inheritFrom'):raise ValueError('Nested provider inheritance is not supported')
            row=dict(value,**row)
        if row.get("type","command")=="command":
            row["program"]=shutil.which(os.path.expandvars(str(row.get("program","")))) or os.path.expandvars(str(row.get("program","")))
            row["arguments"]=[os.path.expandvars(str(value)) for value in row.get("arguments",[])]
        active.append(row)
    digest=hashlib.sha256(json.dumps(active,sort_keys=True,ensure_ascii=False).encode()).hexdigest()
    return active,digest

def audit(path):
    rows,digest=load_policy(path)
    capabilities=[]
    for row in rows:
        kind=row.get('type','command')
        target=' '.join([str(row.get('program','')),*map(str,row.get('arguments',[]))]) if kind=='command' else str(row.get('url','')) if kind=='http' else ' '.join(str(row.get(k,'')) for k in ('service','objectPath','method'))
        capabilities.append(dict(type=kind,target=target,sendRootPaths=bool(row.get('includeRoots',False))))
    return dict(digest=digest,capabilities=capabilities)

if __name__=='__main__':
    try:print(json.dumps(audit(sys.argv[1]),ensure_ascii=False))
    except (OSError,ValueError,IndexError,TypeError) as error:print(json.dumps(dict(error=str(error)),ensure_ascii=False));sys.exit(1)
