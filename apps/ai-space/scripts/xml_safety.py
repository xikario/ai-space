"""Reject DTD/entity declarations before handing XML to older system Expat."""
import io
import re
from xml.etree import ElementTree as _ET

FORBIDDEN = re.compile(br'<!\s*(?:DOCTYPE|ENTITY)\b', re.I)
MAX_XML_BYTES = 128 * 1024 * 1024


class CheckedStream:
    def __init__(self, source):
        self.source = source
        self.tail = b''
        self.total = 0

    def read(self, size=-1):
        value = self.source.read(size)
        self.total += len(value)
        if self.total > MAX_XML_BYTES:
            raise ValueError('xml_input_limit:128_MiB')
        # Removing NULs also detects declarations in UTF-16/32 XML.
        checked = self.tail + value.replace(b'\0', b'')
        if FORBIDDEN.search(checked):
            raise ValueError('xml_dtd_not_supported')
        self.tail = checked[-64:]
        return value


def iterparse(source, events=('end',)):
    return _ET.iterparse(CheckedStream(source), events=events)


def fromstring(value):
    raw = value.encode('utf-8') if isinstance(value, str) else value
    CheckedStream(io.BytesIO(raw)).read()
    return _ET.fromstring(value)


ParseError = _ET.ParseError


def member(archive, name):
    if archive.getinfo(name).file_size > MAX_XML_BYTES:
        raise ValueError('xml_input_limit:128_MiB')
    with archive.open(name) as stream:
        return CheckedStream(stream).read(MAX_XML_BYTES + 1)
