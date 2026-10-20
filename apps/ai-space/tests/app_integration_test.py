"""A real standalone process, private X server/bus and a disposable HOME."""
import ast
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import struct
import zlib
import re
import shutil

binary = Path(sys.argv[1]).resolve()
locked_migration = '--locked-migration' in sys.argv or '--cancel-migration' in sys.argv
cancel_migration = '--cancel-migration' in sys.argv
window_ui = '--window-ui' in sys.argv
def checked(value, message):
    if not value:
        raise AssertionError(message)
    print('PASS:', message, flush=True)
def call(method, *arguments):
    result = subprocess.check_output(['gdbus', 'call', '--session', '--timeout', '5',
        '--dest', 'org.ukui.AiSpace', '--object-path', '/aiSpace',
        '--method', 'org.ukui.AiSpace.' + method, *map(str, arguments)], text=True)
    return ast.literal_eval(result)[0] if method == 'status' else result
def state():
    try:
        return json.loads(call('status'))
    except subprocess.CalledProcessError:
        return {}
def wait(predicate, label, timeout=15):
    deadline = time.monotonic()+timeout
    while time.monotonic()<deadline:
        current = state()
        if predicate(current):
            checked(True, label)
            return current
        time.sleep(.05)
    raise AssertionError(label)

with tempfile.TemporaryDirectory(prefix='ai-space-app-') as directory:
    root=Path(directory);home=root/'home';config=home/'.config';cache=home/'.cache';data=home/'.local/share'
    documents=home/'documents';documents.mkdir(parents=True)
    (config/'kylin').mkdir(parents=True);(config/'kyfences').mkdir();(cache/'kylin/ukui-fences/smart-space').mkdir(parents=True)
    (data/'ukui-fences/knowledge').mkdir(parents=True)
    items=[]
    for name,content in [('银行方案.txt','宁波银行 部署要求'),('银行进展.txt','宁波银行 本周进度'),('其他项目.txt','其他项目材料')]:
        folder=documents/('group-b' if name=='其他项目.txt' else 'group-a');folder.mkdir(exist_ok=True)
        path=folder/name;path.write_text(content)
        items.append(dict(path=str(path),root=str(documents),name=name,category='documents',content=content,isDir=False))
    legacy=config/'kylin/ukui-fences.ini'
    legacy.write_text('[smartSpace]\nroots='+str(documents)+'\nthemeMode=3\nglassOptics=true\ndefaultHidden=true\nautoStart=true\nposition=@Point(100 100)\nsize=@Size(920 520)\n')
    index=cache/'kylin/ukui-fences/smart-space/index.json';index.write_text(json.dumps(dict(items=items,generatedAt='2026-10-19T00:00:00Z'),ensure_ascii=False))
    inherited=config/'kyfences/provider-details.json';inherited.write_text('{"type":"http","url":"https://example.invalid/disabled"}')
    providers=config/'kyfences/smart-space-providers.json';providers.write_text('{"providers":[{"enabled":false,"inheritFrom":"provider-details.json"}]}')
    old_db=data/'ukui-fences/knowledge/smart-space-knowledge.sqlite'
    connection=sqlite3.connect(old_db);connection.execute('PRAGMA journal_mode='+('DELETE' if locked_migration else 'WAL'))
    connection.execute('CREATE TABLE migration_guard(value TEXT)');connection.execute("INSERT INTO migration_guard VALUES ('committed WAL snapshot')");connection.commit()
    if locked_migration:connection.execute('BEGIN EXCLUSIVE')
    hashes={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in (legacy,index,providers,inherited)}
    wallpaper=home/'test-wallpaper.png'
    def chunk(kind, payload):
        return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload)&0xffffffff)
    wallpaper.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,8,2,0,0,0))+
        chunk(b'IDAT',zlib.compress(b'\x00\x22\x88\x77\x33\x77\x99'*2))+chunk(b'IEND',b''))
    env=dict(os.environ,HOME=str(home),XDG_CONFIG_HOME=str(config),XDG_CACHE_HOME=str(cache),XDG_DATA_HOME=str(data),
        AI_SPACE_ROOTS=str(documents),AI_SPACE_WALLPAPER=str(wallpaper),AI_SPACE_OCR='0',AI_SPACE_EXPLICIT_SHOW='1',AI_SPACE_TEST_NO_BOOTSTRAP='1',
        UKUI_FENCES_GLASS_NO_GL='1',UKUI_LIQUID_GLASS_NO_GL='1',GSETTINGS_SCHEMA_DIR='/usr/share/glib-2.0/schemas',GIO_USE_VFS='local')
    log=root/'app.log'
    process=subprocess.Popen([str(binary),'--show'],env=env,stdout=log.open('w'),stderr=subprocess.STDOUT)
    try:
        ready=wait(lambda s:s.get('ready') and s.get('visible') and s.get('indexedEntries')==3 and s.get('glassBackend')=='solid',
            'independent window restores legacy index without Fences')
        pid=ready['pid'];checked(not ready.get('indexBusy'),'opening does not start a document scan')
        checked(ready['navigationGroups']==2,'startup groups files by their actual parent paths')
        if locked_migration:
            checked(ready['migrationPending'],'usable window is ready while the old DB is locked')
            call('search','宁波银行')
            wait(lambda s:s.get('results')==2 and s.get('migrationPending'),'file snapshot search stays responsive during knowledge migration')
            checked(not (data/'ai-space/knowledge/smart-space-knowledge.sqlite').exists(),'pending migration publishes no partial database')
            if cancel_migration:
                call('quit');checked(process.wait(timeout=10)==0,'closing during a blocked import cancels its helper normally')
                checked(not (data/'ai-space/knowledge/smart-space-knowledge.sqlite').exists(),'cancelled import leaves no incomplete target')
                connection.rollback()
                process=subprocess.Popen([str(binary),'--show'],env=env,stdout=log.open('a'),stderr=subprocess.STDOUT)
                ready=wait(lambda s:s.get('ready') and not s.get('migrationPending') and s.get('indexedEntries')==3,'interrupted migration retries successfully on restart')
                pid=ready['pid']
            else:connection.commit()
        wait(lambda s:s.get('ready') and not s.get('migrationPending'),'background knowledge import finishes')
        checked(ready['glassBackend']=='solid' and not ready['glassOptics'] and not ready['usedGpu'] and ready['maximizable'],'solid theme has no glass shader and supports window controls')
        call('toggleMaximize')
        wait(lambda s:s.get('maximized'),'window maximizes')
        call('toggleMaximize')
        wait(lambda s:not s.get('maximized'),'window restores from maximized')
        if window_ui:
            checked(bool(shutil.which('xdotool')), 'native mouse/keyboard test tool is available')
            def pointer(x, y, *commands):
                current=state()
                info=subprocess.check_output(['xwininfo','-id',current['windowId']],text=True)
                native_width=int(re.search(r'Width: (\d+)',info).group(1))
                ratio=native_width/current['width']
                subprocess.run(['xdotool','mousemove','--sync','--window',current['windowId'],
                    str(round(x*ratio)),str(round(y*ratio)),*commands],check=True)
            current=state()
            pointer(current['width']-65,17,'click','1')
            wait(lambda s:s.get('maximized'),'clicking the vector maximize button maximizes the actual window')
            current=state()
            pointer(current['width']-65,17,'click','1')
            wait(lambda s:not s.get('maximized'),'clicking the restore glyph restores the actual window')
            before=state()
            pointer(400,18,'mousedown','1')
            pointer(460,48,'mouseup','1')
            wait(lambda s:s.get('x')!=before['x'] or s.get('y')!=before['y'], 'dragging the title bar moves the window')
            call('resizeWindow',1200,680)
            wait(lambda s:s.get('width')>=1100,'wide window is ready')
            time.sleep(.2)
            pointer(state()['width']-80,160,'click','1')
            wait(lambda s:s.get('previewVisible') and s.get('previewDocked'),'real preview button opens a third column beside results')
            call('resizeWindow',760,520)
            wait(lambda s:s.get('previewVisible') and not s.get('previewDocked'),'narrow resize keeps the preview available without three cramped columns')
            subprocess.run(['xdotool','key','Escape'],check=True)
            wait(lambda s:not s.get('previewVisible'),'Escape closes the focused preview')
            call('showSettings')
            wait(lambda s:s.get('settingsVisible'),'settings remain reachable in the redesigned title bar')
            subprocess.run(['xdotool','key','ctrl+k'],check=True)
            wait(lambda s:not s.get('settingsVisible'),'Ctrl+K returns from settings to search')
            pointer(state()['width']-107,17,'click','1')
            wait(lambda s:s.get('minimized'),'native minimize button minimizes the window')
            call('showWindow')
            wait(lambda s:s.get('visible') and not s.get('minimized'),'application restores after native minimize')
        with sqlite3.connect(data/'ai-space/knowledge/smart-space-knowledge.sqlite') as copy:
            checked(copy.execute('SELECT value FROM migration_guard').fetchone()[0]=='committed WAL snapshot','consistent SQLite WAL backup imported')
        checked(all(hashlib.sha256(p.read_bytes()).hexdigest()==h for p,h in hashes.items()),'legacy config, index and providers unchanged')
        new_providers=config/'ai-space/providers.json'
        new_include=json.loads(new_providers.read_text())['providers'][0]['inheritFrom']
        checked((new_providers.parent/new_include).read_bytes()==inherited.read_bytes(),'relative Provider inheritance survives relocation')
        checked(not (config/'autostart/ai-space.desktop').exists(),'migration does not silently add a login task')
        call('setAutoStart','true')
        checked((config/'autostart/ai-space.desktop').is_file() and state()['autoStart'],'explicit login startup creates an independent desktop entry')
        call('setAutoStart','false')
        checked(not (config/'autostart/ai-space.desktop').exists() and not state()['autoStart'],'disabling login startup removes only the AI Space entry')
        native=subprocess.check_output(['xprop','-id',ready['windowId'],'_NET_WM_WINDOW_TYPE','_NET_WM_ICON','WM_CLASS'],text=True)
        checked('_NET_WM_WINDOW_TYPE_NORMAL' in native and 'ai-space' in native and '_NET_WM_ICON(CARDINAL)' in native,'normal application window has its own taskbar identity and icon')
        subprocess.run([str(binary),'--show'],env=env,check=True,timeout=10)
        checked(state()['pid']==pid,'second launch reuses the existing instance')
        call('setGlassOptics','true')
        checked(not state()['glassOptics'] and state()['glassBackend']=='solid','legacy effect request cannot re-enable glass rendering')
        builds=state()['materialBuilds']
        call('search','宁波银行')
        wait(lambda s:s.get('results')==2 and s.get('navigationGroups')==1,'actual search UI filters imported documents and navigation groups')
        checked(state()['materialBuilds']==builds,'search does not build glass materials')
        call('hideWindow');wait(lambda s:not s.get('visible') and s.get('minimized') and not s.get('edgeHidden'),'legacy hide minimizes to the normal taskbar')
        call('showWindow');wait(lambda s:s.get('visible'),'hidden application can be restored')
        call('hideToEdge');wait(lambda s:s.get('minimized') and not s.get('edgeHidden') and s.get('width',0)>=620,'retired edge command minimizes without creating a star entry')
        call('revealFromEdge');wait(lambda s:s.get('visible') and not s.get('minimized') and not s.get('edgeHidden'),'retired reveal command restores the normal window')
        call('showSettings');wait(lambda s:s.get('settingsVisible'),'settings page opens')
        if window_ui:
            normal_size=(state()['width'],state()['height'])
            call('maximizeWindow')
            wait(lambda s:s.get('maximized'),'settings can maximize without replacing normal saved geometry')
        call('quit');checked(process.wait(timeout=10)==0,'application and its workers shut down normally')
        # A second start uses its own migrated data without recopying changes
        # from a legacy application that continues running independently.
        legacy.write_text(legacy.read_text().replace('roots=','ignoredRoots='))
        env['AI_SPACE_WALLPAPER']=str(home/'missing-wallpaper.png')
        process=subprocess.Popen([str(binary),'--hide'],env=env,stdout=log.open('a'),stderr=subprocess.STDOUT)
        wait(lambda s:s.get('ready') and not s.get('visible') and s.get('minimized') and s.get('indexedEntries')==3,'legacy background start minimizes and independent data survives restart')
        if window_ui:
            restored=state()
            checked(abs(restored['width']-normal_size[0])<=2 and abs(restored['height']-normal_size[1])<=2,
                'closing maximized preserves the normal window size on restart')
        call('showWindow')
        wait(lambda s:s.get('glassBackend')=='solid' and s.get('materialBuilds')==0 and not s.get('usedGpu'),'solid surface is independent of system wallpaper')
        call('quit');checked(process.wait(timeout=10)==0,'minimized startup instance exits normally')
        current_config=config/'xikario/ai-space.ini'
        text=current_config.read_text()
        text=text.replace('[smartSpace]','[smartSpace]\ndefaultHidden=true',1)
        current_config.write_text(text)
        env.pop('AI_SPACE_EXPLICIT_SHOW',None)
        process=subprocess.Popen([str(binary),'--autostart'],env=env,stdout=log.open('a'),stderr=subprocess.STDOUT)
        wait(lambda s:s.get('ready') and s.get('visible') and not s.get('indexLoading'),'login start opens a normal window')
        time.sleep(.35)
        checked(not state()['edgeHidden'] and not state()['minimized'] and state()['width']>=620,
            'stale defaultHidden=true cannot retract the login window')
        checked('defaultHidden=' not in current_config.read_text(),'obsolete startup hiding preference is removed')
        if window_ui:
            pointer(state()['width']-22,17,'click','1')
            checked(process.wait(timeout=10)==0,'native title close button exits normally')
        else:
            call('quit');checked(process.wait(timeout=10)==0,'hidden instance exits cleanly')
    except BaseException:
        print(log.read_text(errors='replace'),file=sys.stderr)
        raise
    finally:
        connection.close()
        if process.poll() is None:
            process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait()
