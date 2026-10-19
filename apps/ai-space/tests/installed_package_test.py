"""Install an independent prefix, then exercise its workers and real GUI process."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

build=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='ai-space-package-') as temporary:
    root=Path(temporary)
    prefix=root/'prefix with spaces'
    subprocess.run(['cmake','--install',str(build),'--prefix',str(prefix)],check=True,stdout=subprocess.DEVNULL)
    desktop=(prefix/'share/applications/ai-space.desktop').read_text()
    assert 'Exec="'+str(prefix/'bin/ai-space')+'" --show' in desktop
    assert 'Icon='+str(prefix/'share/icons/hicolor/128x128/apps/ai-space.png') in desktop
    document=root/'办公材料.docx'
    with zipfile.ZipFile(document,'w') as archive:
        archive.writestr('word/document.xml','<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>AI空间独立安装验证 宁波银行</w:t></w:r></w:p></w:body></w:document>')
    index=root/'index.json'
    index.write_text(json.dumps({'items':[{'path':str(document),'name':document.name,'isDir':False}]}))
    database=root/'knowledge.sqlite'
    environment=dict(os.environ,PYTHONPATH='',PYTHONNOUSERSITE='1',PATH=str(prefix/'bin')+os.pathsep+os.environ['PATH'])
    worker=prefix/'libexec/ai-space/smart_space_knowledge.py'
    for arguments in [('build','--index-json',str(index),'--no-ocr'),('stats',),('search','--query','宁波银行')]:
        output=subprocess.check_output([sys.executable,str(worker),arguments[0],'--db',str(database),*arguments[1:]],env=environment,text=True,timeout=30)
        value=json.loads(output.strip().splitlines()[-1])
        assert not value.get('error'),value
        if arguments[0]=='search':
            assert value['results'][0]['path']==str(document),value
    query=prefix/'share/ai-space/skills/ai-space-index-query/scripts/query.py'
    output=subprocess.check_output([sys.executable,str(query),'--db',str(database),'search','--query','宁波银行'],env=environment,text=True,timeout=10)
    assert json.loads(output.strip().splitlines()[-1])['results']
    print('PASS: installed desktop entry, structured extraction, SQLite search and Skill use the standalone package',flush=True)
    subprocess.run([sys.executable,str(Path(__file__).with_name('app_integration_test.py')),str(prefix/'bin/ai-space')],env=environment,check=True,timeout=60)
