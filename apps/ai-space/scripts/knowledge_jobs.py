#!/usr/bin/env python3
"""Explicit, resumable manifest -> text -> optional resource-limited OCR job."""
import argparse
import contextlib
import fcntl
import json
import gzip
import struct
import resource
from ocr_budget import available_memory,ResourcePressureError
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import tempfile
from knowledge_library import storage_plan,guard_storage,GIB,emit,registry_write,manifest,open_readonly,stats,document_ids,ignored_ids


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--directory',required=True)
    parser.add_argument('--roots-json',required=True)
    parser.add_argument('--previous-db')
    parser.add_argument('--provider-config',default='')
    parser.add_argument('--provider-approval',default='')
    parser.add_argument('--chunk-size',type=int,default=1200)
    parser.add_argument('--overlap',type=int,default=120)
    parser.add_argument('--exclude-roots-json',default='[]')
    parser.add_argument('--include-extensions-json',default='[]')
    parser.add_argument('--ocr',action='store_true')
    modes=parser.add_mutually_exclusive_group()
    modes.add_argument('--ocr-only',action='store_true')
    modes.add_argument('--manifest-only',action='store_true')
    modes.add_argument('--text-only',action='store_true')
    modes.add_argument('--retry-documents-json',default='',help='retry only selected existing failed/ignored document IDs')
    parser.add_argument('--retry-db',default='',help='exact personal database selected in the failure list')
    parser.add_argument('--reserve-gib',type=float,default=2)
    parser.add_argument('--max-gib',type=float,default=32)
    parser.add_argument('--ocr-cpu',type=int,default=25)
    parser.add_argument('--ocr-memory',type=int,default=1024)
    args=parser.parse_args()
    process=None
    temp_session=None
    try:
        directory=Path(args.directory).expanduser().resolve()
        if args.retry_db and not args.retry_documents_json:raise ValueError('--retry-db requires a selected-document retry')
        storage_plan(directory,0,args.reserve_gib,args.max_gib)
        roots=json.loads(args.roots_json)
        if not args.retry_documents_json and (not isinstance(roots,list) or not roots or any(not isinstance(p,str) or not Path(p).is_dir() for p in roots)):
            raise ValueError('Select existing source folders before building')
        if any(Path(p).resolve()==directory or directory in Path(p).resolve().parents for p in roots):
            raise ValueError('Choose a dedicated knowledge directory that does not contain your source folders')
        # Prevent recursive self-indexing and accidentally pruning a shared pack.
        excludes=json.loads(args.exclude_roots_json)+[str(directory)]
        directory.mkdir(parents=True,exist_ok=True)
        lock=open(str(directory/'.pipeline.lock'),'a')
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        catalog=directory/'catalog';catalog.mkdir(exist_ok=True)
        temporary=directory/'temporary';temporary.mkdir(exist_ok=True)
        database=directory/'smart-space-knowledge.sqlite'
        if args.retry_db:
            database=Path(args.retry_db).expanduser().resolve()
            if database.parent!=directory:raise ValueError('Retry database must be on the selected knowledge storage directory')
        index=catalog/'index.json'
        index_stream=Path(str(index)+'.ui.bin.gz')
        temp_session=tempfile.TemporaryDirectory(prefix='job-',dir=str(temporary))
        env=dict(os.environ,TMPDIR=temp_session.name,AI_SPACE_STORAGE_DIRECTORY=str(directory),
            AI_SPACE_RESERVE_GIB=str(args.reserve_gib),AI_SPACE_MAX_GIB=str(args.max_gib),
            AI_SPACE_OCR_CPU_PERCENT=str(args.ocr_cpu),AI_SPACE_OCR_MEMORY_MB=str(args.ocr_memory),AI_SPACE_OCR_CACHE_DIR=str(directory/'ocr-cache'))
        scripts=Path(__file__).resolve().parent
        def paused(signum,frame):raise InterruptedError('Job paused; completed documents are retained')
        signal.signal(signal.SIGTERM,paused)
        try:os.nice(15)
        except OSError:pass
        def run(stage,command):
            nonlocal process
            emit(task='pipeline',stage=stage,phase='start')
            storage_plan(directory,0,args.reserve_gib,args.max_gib)
            memory=max(256,min(8192,args.ocr_memory))*1024**2
            free=available_memory()
            if free is not None and free<memory+256*1024**2:
                raise ResourcePressureError('系统可用内存不足，建库已暂停并保留进度；可降低工作进程预算或稍后继续')
            def worker_limits():resource.setrlimit(resource.RLIMIT_AS,(memory,memory))
            process=subprocess.Popen([sys.executable,*map(str,command)],env=env,preexec_fn=worker_limits)
            while process.poll() is None:
                guard_storage(database,args.reserve_gib,args.max_gib)
                # Catalogs, temporary OCR outputs and SQLite WAL share this
                # volume; reserve is checked even during a long document.
                storage_plan(directory,0,args.reserve_gib,args.max_gib)
                time.sleep(.2)
            if process.returncode:raise RuntimeError('%s failed (exit %s); completed data retained'%(stage,process.returncode))
            process=None
            state=dict(stage=stage,completed_at=time.strftime('%Y-%m-%dT%H:%M:%S'))
            pending=catalog/'job.json.partial'
            pending.write_text(json.dumps(state));os.replace(pending,catalog/'job.json')
        def preserve_previous():
            if args.previous_db:
                previous=Path(args.previous_db).resolve()
                if previous.is_file() and previous!=database:
                    with contextlib.closing(open_readonly(previous)) as old:
                        identity=manifest(old,previous)
                    stat=previous.stat()
                    row=dict(identity,path=str(previous),bytes=stat.st_size,mtime_ns=stat.st_mtime_ns,read_only=True,sha256='')
                    with registry_write() as libraries:
                        if not any(r['path']==str(previous) for r in libraries['libraries']):libraries['libraries'].append(row)
        if args.retry_documents_json:
            selected=document_ids(args.retry_documents_json)
            with contextlib.closing(open_readonly(database)) as source:
                if manifest(source,database).get('format')!='legacy-sqlite':raise ValueError('AIKB libraries are read-only; retry on the source machine')
                slots=','.join('?' for ident in selected);skipped=ignored_ids(source)
                rows=source.execute('SELECT d.id,d.path,d.root,d.name,d.category,d.size,d.modified_ms,e.status FROM documents d JOIN extraction_state e ON e.document_id=d.id WHERE d.id IN ('+slots+')',selected).fetchall()
                if len(rows)!=len(selected) or any(row[-1] not in ('failed','missing') and row[0] not in skipped for row in rows):
                    raise ValueError('Selected failures changed; refresh the list')
                items=[dict(path=path,root=root,name=name,category=category,size=size,modifiedMs=modified,isDir=False) for ident,path,root,name,category,size,modified,state in rows]
            retry_index=Path(temp_session.name)/'retry.json'
            retry_index.write_text(json.dumps(dict(items=items,generatedAt=time.strftime('%Y-%m-%dT%H:%M:%S'))))
            command=[scripts/'smart_space_knowledge.py','build','--index-json',retry_index,'--db',database,'--force','--ocr' if args.ocr else '--no-ocr','--chunk-size',args.chunk_size,'--overlap',args.overlap]
            for ident in selected:command+=['--retry-document-id',ident]
            run('retry',command)
            with contextlib.closing(open_readonly(database)) as source:
                failures=source.execute("SELECT count(*) FROM extraction_state WHERE document_id IN ("+slots+") AND status IN ('failed','missing')",selected).fetchone()[0]
            emit(done=True,task='pipeline',phase='retry-complete',db=str(database),retried=len(selected),retryFailures=failures)
            return 0
        if not args.ocr_only and not args.text_only:
            run('manifest',[scripts/'smart_space_indexer.py','--roots-json',args.roots_json,
                '--exclude-roots-json',json.dumps(excludes),'--include-extensions-json',args.include_extensions_json,
                '--output',index,'--fast-full','--metadata-only','--max-items','0','--no-tags',
                '--resume-state',catalog/'manifest.resume.jsonl',*(['--provider-config',args.provider_config,'--provider-approval',args.provider_approval] if args.provider_config else [])])
        if not index_stream.exists():raise ValueError('请先完成第 1 步全量扫描，再执行文本建库或 OCR')
        if args.ocr_only and not database.exists():raise ValueError('请先完成第 1 步扫描和第 2 步文本建库，再执行 OCR')
        from smart_space_knowledge import inventory_metadata,iter_stream
        inventory=inventory_metadata(index_stream,index)
        if args.text_only or args.ocr_only:
            if {str(Path(p).resolve()) for p in inventory.get('roots',[])}!={str(Path(p).resolve()) for p in roots}:
                raise ValueError('资料目录已改变，请重新执行第 1 步全量扫描')
            paths=lambda values:{os.path.abspath(os.path.expanduser(p)) for p in values}
            extensions=lambda values:{p.strip().lower().lstrip('.') for p in values if p.strip().lstrip('.')}
            if (paths(inventory.get('excludedRoots',[]))!=paths(excludes)
                    or extensions(inventory.get('includedExtensions',[]))!=extensions(json.loads(args.include_extensions_json))):
                raise ValueError('扫描范围已改变，请重新执行第 1 步全量扫描')
        if not args.ocr_only:
            has_files=any(not item['is_dir'] for total,item in iter_stream(index_stream))
            if not has_files and database.exists() and stats(database)['documents']:
                raise RuntimeError('Empty inventory: refused to prune an existing knowledge library; use a new storage folder for an empty library')
            if args.manifest_only:
                emit(done=True,task='pipeline',phase='manifest-complete',stage='manifest',message='文件清单已完成，请继续第 2 步文本建库')
                return 0
            run('text',[scripts/'smart_space_knowledge.py','build','--index-json',index,
                '--index-stream',index_stream,'--db',database,'--no-ocr','--chunk-size',args.chunk_size,'--overlap',args.overlap])
        preserve_previous()
        emit(task='pipeline',stage='text',phase='ready',db=str(database))
        if args.ocr or args.ocr_only:
            if not database.exists():raise ValueError('Run manifest and text stages before OCR')
            run('ocr',[scripts/'smart_space_knowledge.py','build','--index-json',index,
                '--index-stream',index_stream,'--db',database,'--ocr','--ocr-only','--chunk-size',args.chunk_size,'--overlap',args.overlap])
        summary=stats(database)
        emit(done=True,task='pipeline',phase='complete',ocr=args.ocr or args.ocr_only,db=str(database),documents=summary['documents'],extractionStates=summary['extractionStates'])
        return 0
    except (OSError,ValueError,RuntimeError,EOFError,struct.error) as exc:
        emit(done=True,task='pipeline',error=str(exc));return 1
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=3)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        if temp_session:temp_session.cleanup()


if __name__=='__main__':sys.exit(main())
