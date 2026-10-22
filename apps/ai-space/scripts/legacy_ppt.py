"""Read current slide text from PowerPoint 97–2003 OLE streams.

Original implementation of MS-PPT §§2.1.2, 2.3.2–4, 2.4.14, 2.9.76.
Never scan binary strings: superseded edits, removed slides, masters, notes,
embedded objects and pictures are not presentation slide text.
"""
import re
import struct
import time

PARSER_VERSION = 'legacy-ppt-1'
MAX_STREAM_BYTES = 128 * 1024**2
MAX_RECORDS = 1000000
MAX_DEPTH = 64
MAX_SECONDS = 30


class LegacyPresentation:
    def __init__(self, ole):
        self.started = time.monotonic()
        self.records = 0
        self.data = self._read(ole, 'PowerPoint Document', MAX_STREAM_BYTES)
        user = self._read(ole, 'Current User', 4096)
        if len(user) < 28 or struct.unpack_from('<H', user, 2)[0] != 0x0FF6:
            raise ValueError('旧版演示文稿缺少有效的当前版本信息；请另存后重试')
        token, current = struct.unpack_from('<II', user, 12)
        if token == 0xF3D1C4DF:
            raise ValueError('旧版演示文稿已加密；请解除加密后重新处理')
        if token != 0xE391C05F:
            raise ValueError('旧版演示文稿的文件标识无效；请检查文件是否损坏')
        self.directory = {}
        visited = set()
        self.document_id = None
        while True:
            if current in visited or len(visited) >= 4096:
                raise ValueError('旧版演示文稿的版本引用循环或过多；请另存后重试')
            visited.add(current)
            _flags, kind, start, end = self.record(current, len(self.data))
            if kind != 0x0FF5 or end-start not in (28, 32):
                raise ValueError('旧版演示文稿的版本记录损坏；请另存后重试')
            previous, directory, document = struct.unpack_from('<III', self.data, start+8)
            if end-start == 32 and struct.unpack_from('<I', self.data, start+28)[0]:
                raise ValueError('旧版演示文稿已加密；请解除加密后重新处理')
            if self.document_id is None:
                self.document_id = document
            self._add_directory(directory)
            if not previous:
                break
            if previous >= current:
                raise ValueError('旧版演示文稿的版本引用无效；请另存后重试')
            current = previous

    @staticmethod
    def _read(ole, name, maximum):
        if not ole.exists(name):
            raise ValueError('旧版演示文稿缺少必要的数据流；请另存后重试')
        size = ole.get_size(name)
        if size > maximum:
            raise ValueError('旧版演示文稿的数据流超过解析内存限额；请拆分或另存后重试')
        with ole.openstream(name) as stream:
            data = stream.read(maximum+1)
        if len(data) != size:
            raise ValueError('旧版演示文稿的数据流不完整；请检查或重新保存文件')
        return data

    def record(self, offset, limit):
        self.records += 1
        if self.records > MAX_RECORDS or time.monotonic()-self.started > MAX_SECONDS:
            raise ValueError('旧版演示文稿解析超过资源或时间限额；请拆分或另存后重试')
        if offset < 0 or offset+8 > limit or limit > len(self.data):
            raise ValueError('旧版演示文稿的记录头不完整；请检查或重新保存文件')
        flags, kind, size = struct.unpack_from('<HHI', self.data, offset)
        end = offset+8+size
        if end > limit:
            raise ValueError('旧版演示文稿的数据记录超出文件边界；请检查或重新保存文件')
        return flags, kind, offset+8, end

    def children(self, start, end):
        while start < end:
            record = self.record(start, end)
            yield record
            start = record[3]

    def _add_directory(self, offset):
        _flags, kind, start, end = self.record(offset, len(self.data))
        if kind != 0x1772:
            raise ValueError('旧版演示文稿的对象索引无效；请另存后重试')
        while start < end:
            if end-start < 4:
                raise ValueError('旧版演示文稿的对象索引不完整')
            header = struct.unpack_from('<I', self.data, start)[0]
            ident, count = header & 0xFFFFF, header >> 20
            start += 4
            if not ident or not count or ident+count > 0x100000 or start+count*4 > end:
                raise ValueError('旧版演示文稿的对象索引范围无效')
            for index in range(count):
                target = struct.unpack_from('<I', self.data, start+index*4)[0]
                if target+8 > len(self.data):
                    raise ValueError('旧版演示文稿的对象位置超出文件边界')
                # Walk newest to oldest; an earlier edit must not overwrite it.
                self.directory.setdefault(ident+index, target)
            start += count*4
            if len(self.directory) > 200000:
                raise ValueError('旧版演示文稿的对象数量超过解析限额')

    def object(self, ident, expected):
        if ident not in self.directory:
            raise ValueError('旧版演示文稿缺少当前页面的对象引用；请另存后重试')
        flags, kind, start, end = self.record(self.directory[ident], len(self.data))
        if kind != expected or flags & 15 != 15:
            raise ValueError('旧版演示文稿的当前页面结构无效；请另存后重试')
        return start, end

    def text(self, kind, start, end):
        try:
            # TextBytesAtom stores the low byte of Unicode, not the host locale.
            value = self.data[start:end].decode('utf-16le' if kind == 0x0FA0 else 'latin1')
        except UnicodeError:
            raise ValueError('旧版演示文稿的文字编码损坏；请另存后重试')
        value = value.replace('\r\n', '\n').replace('\r', '\n').replace('\v', '\n')
        return re.sub(r'[\x00-\x08\x0c\x0e-\x1f\x7f-\x9f]', '', value).strip()

    def _slide_list(self):
        start, end = self.object(self.document_id, 0x03E8)
        found = False
        for flags, kind, begin, finish in self.children(start, end):
            # Instance 1 is master slides; instance 2 is speaker notes.
            if kind != 0x0FF0 or flags >> 4 != 0:
                continue
            if found:
                raise ValueError('旧版演示文稿包含重复的当前页面列表；请另存后重试')
            found = True
            current = None
            for _flags, child, at, stop in self.children(begin, finish):
                if child == 0x03F3:
                    if stop-at < 20:
                        raise ValueError('旧版演示文稿的页面引用不完整')
                    if current is not None:
                        yield current
                    current = (struct.unpack_from('<I', self.data, at)[0], [])
                elif child == 0x0F9F and current is not None:
                    current[1].append([])
                elif child in (0x0FA0, 0x0FA8) and current is not None:
                    if not current[1]:
                        raise ValueError('旧版演示文稿的占位文字索引无效')
                    value = self.text(child, at, stop)
                    if value:
                        current[1][-1].append(value)
            if current is not None:
                yield current

    def _shape_text(self, start, end, outline, referenced, depth=0, textbox=False):
        if depth > MAX_DEPTH:
            raise ValueError('旧版演示文稿的图形嵌套超过解析限额')
        for _flags, kind, at, stop in self.children(start, end):
            if textbox and kind == 0x0F9E:
                if stop-at != 4:
                    raise ValueError('旧版演示文稿的占位文字引用不完整')
                index = struct.unpack_from('<I', self.data, at)[0]
                if index >= len(outline):
                    raise ValueError('旧版演示文稿的占位文字引用超出范围')
                referenced.add(index)
                yield from outline[index]
            elif textbox and kind in (0x0FA0, 0x0FA8):
                value = self.text(kind, at, stop)
                if value:
                    yield value
            elif kind in (0x040C, 0xF002, 0xF003, 0xF004, 0xF00D):
                # Only descend slide drawings/groups/shapes/textboxes. Unknown
                # binary records are skipped, never guessed to contain text.
                yield from self._shape_text(at, stop, outline, referenced,
                                            depth+1, kind == 0xF00D)

    def slides(self):
        for number, (ident, outline) in enumerate(self._slide_list(), 1):
            start, end = self.object(ident, 0x03EE)
            referenced = set()
            text = list(self._shape_text(start, end, outline, referenced))
            # Current placeholders can exist without a matching drawing shape.
            # They still belong to this slide, unlike orphan persist objects.
            for index, pieces in enumerate(outline):
                if index not in referenced:
                    text.extend(pieces)
            yield number, '\n'.join(text)
