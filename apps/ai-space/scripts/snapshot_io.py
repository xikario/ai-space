"""Streaming canonical JSON snapshots and disposable disk-backed reuse maps."""
from collections.abc import MutableMapping
import contextlib
import io
import json
from pathlib import Path
import sqlite3
import tempfile

from text_safety import open_regular_binary

MAX_VALUE = 16 * 1024 * 1024


class JsonReader:
    def __init__(self, source):
        self.source = source
        self.buffer = ''
        self.position = 0
        self.eof = False
        self.decoder = json.JSONDecoder()

    def more(self):
        self.buffer = self.buffer[self.position:]
        self.position = 0
        value = self.source.read(65536)
        self.eof = not value
        self.buffer += value

    def peek(self):
        while True:
            while self.position < len(self.buffer) and self.buffer[self.position].isspace():
                self.position += 1
            if self.position < len(self.buffer):
                return self.buffer[self.position]
            if self.eof:
                return ''
            self.more()

    def take(self, expected):
        if self.peek() != expected:
            raise ValueError('Invalid snapshot JSON structure')
        self.position += 1

    def value(self):
        self.peek()
        while True:
            try:
                value, end = self.decoder.raw_decode(self.buffer, self.position)
                # A number may continue in the next input chunk.
                if end == len(self.buffer) and not self.eof:
                    self.more()
                    continue
                self.position = end
                return value
            except json.JSONDecodeError:
                if self.eof or len(self.buffer) - self.position > MAX_VALUE:
                    raise ValueError('Invalid or oversized snapshot JSON value')
                self.more()


def records(path, header_only=False):
    with open_regular_binary(path) as raw, io.TextIOWrapper(raw, encoding='utf-8-sig') as source:
        reader = JsonReader(source)
        reader.take('{')
        seen = set()
        generated = False
        while reader.peek() != '}':
            key = reader.value()
            if not isinstance(key, str) or key in seen:
                raise ValueError('Invalid or duplicate snapshot metadata key')
            seen.add(key)
            reader.take(':')
            if key == 'items':
                if header_only and generated:
                    return
                reader.take('[')
                while reader.peek() != ']':
                    value = reader.value()
                    if not isinstance(value, dict):
                        raise ValueError('Invalid snapshot item')
                    if not header_only:
                        yield 'item', value
                    if reader.peek() == ']':
                        break
                    reader.take(',')
                reader.take(']')
            else:
                value = reader.value()
                generated = generated or key == 'generation'
                yield key, value
            if reader.peek() == '}':
                break
            reader.take(',')
        reader.take('}')
        if reader.peek():
            raise ValueError('Trailing snapshot JSON data')


def metadata(path):
    return dict(records(path, header_only=True))


class ReuseMap(MutableMapping):
    """Keep large previous bodies off the Python heap during an incremental scan."""
    def __init__(self, directory=None):
        self.temporary = tempfile.TemporaryDirectory(prefix='.ai-space-reuse-', dir=disposable_owner(directory))
        self.owner=hold_owner(self.temporary)
        self.db = sqlite3.connect(str(Path(self.temporary.name) / 'items.sqlite'))
        self.db.executescript('''PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
            PRAGMA cache_size=-4096; PRAGMA temp_store=FILE;
            CREATE TABLE items(path TEXT PRIMARY KEY,json TEXT NOT NULL);''')

    def __getitem__(self, key):
        row = self.db.execute('SELECT json FROM items WHERE path=?', (key,)).fetchone()
        if row is None:
            raise KeyError(key)
        return json.loads(row[0])

    def __setitem__(self, key, value):
        self.db.execute('INSERT OR REPLACE INTO items VALUES(?,?)',
                        (key, json.dumps(value, ensure_ascii=False, separators=(',', ':'))))

    def __delitem__(self, key):
        if not self.db.execute('DELETE FROM items WHERE path=?', (key,)).rowcount:
            raise KeyError(key)

    def __iter__(self):
        for row in self.db.execute('SELECT path FROM items'):
            yield row[0]

    def __len__(self):
        return self.db.execute('SELECT count(*) FROM items').fetchone()[0]

    def close(self):
        if self.db is not None:
            self.db.close()
            self.db = None
            self.temporary.cleanup()
            self.owner.close()

    def __del__(self):
        with contextlib.suppress(Exception):
            self.close()


class RecordList:
    """A replayable on-disk sequence for large inventories and OCR queues."""
    def __init__(self, directory=None):
        self.temporary=tempfile.TemporaryDirectory(prefix='.ai-space-records-',dir=disposable_owner(directory))
        self.owner=hold_owner(self.temporary)
        self.db=sqlite3.connect(str(Path(self.temporary.name)/'records.sqlite'))
        self.db.executescript('''PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
            PRAGMA cache_size=-4096; PRAGMA temp_store=FILE;
            CREATE TABLE records(position INTEGER PRIMARY KEY,json TEXT,size INTEGER,path TEXT);
            CREATE INDEX records_size ON records(size,path);''')
        self.ordered=False
        self.count=0

    def __len__(self):return self.count

    def append(self, value):
        self.db.execute('INSERT INTO records VALUES(?,?,?,?)',
                        (self.count,json.dumps(value,ensure_ascii=False,separators=(',',':')),
                         int(value.get('size',0) or 0),str(value.get('path',''))))
        self.count+=1

    def extend(self, values):
        for value in values:self.append(value)

    def __iter__(self):
        order='size,path COLLATE NOCASE' if self.ordered else 'position'
        for row in self.db.execute('SELECT json FROM records ORDER BY '+order):yield json.loads(row[0])

    def __getitem__(self, position):
        if position<0:position+=self.count
        row=self.db.execute('SELECT json FROM records WHERE position=?',(position,)).fetchone()
        if not row:raise IndexError(position)
        return json.loads(row[0])

    def __setitem__(self, position, value):
        if position<0:position+=self.count
        if not 0<=position<self.count:raise IndexError(position)
        self.db.execute('UPDATE records SET json=?,size=?,path=? WHERE position=?',
                        (json.dumps(value,ensure_ascii=False,separators=(',',':')),
                         int(value.get('size',0) or 0),str(value.get('path','')),position))

    def truncate(self, count):
        self.db.execute('DELETE FROM records WHERE position>=?',(count,));self.count=min(self.count,count)

    def sort_by_size(self):self.ordered=True

    def close(self):
        if self.db is not None:
            self.db.close();self.db=None;self.temporary.cleanup();self.owner.close()

    def __del__(self):
        with contextlib.suppress(Exception):self.close()


_CLEANED = set()

def disposable_owner(directory):
    """Recover only our abandoned scratch caches, never personal libraries."""
    import fcntl
    import shutil
    root=Path(directory) if directory is not None else Path(tempfile.gettempdir())
    key=str(root.resolve())
    if key not in _CLEANED:
        _CLEANED.add(key)
        for path in root.iterdir():
            if not path.name.startswith(('.ai-space-reuse-','.ai-space-records-')) or path.is_symlink() or not path.is_dir():continue
            marker=path/'.owner'
            try:
                if marker.is_symlink():continue
                with marker.open('r+b') as handle:
                    if handle.read(64)!=b'ai-space-disposable-cache-v1':continue
                    try:fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
                    except BlockingIOError:continue
                    shutil.rmtree(path)
            except (OSError,ValueError):pass
    return root


def hold_owner(temporary):
    import fcntl
    marker=Path(temporary.name)/'.owner'
    handle=marker.open('x+b');fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
    handle.write(b'ai-space-disposable-cache-v1');handle.flush()
    return handle
