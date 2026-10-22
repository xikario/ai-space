"""Bounded, regular-file-only text reads shared by preview and body extraction."""
import codecs
from contextlib import contextmanager
import os
from pathlib import Path
import stat

SAMPLE_BYTES = 64 * 1024
TEXT_READ_CHARS = 64 * 1024
DEFAULT_DOCUMENT_CHARS = 8 * 1024 * 1024


@contextmanager
def open_regular_binary(path):
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    if Path(path).is_symlink():
        raise ValueError("non_regular_source:symlink")
    descriptor = os.open(str(path), flags)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise ValueError("non_regular_source:device_or_pipe")
        stream = os.fdopen(descriptor, "rb")
        descriptor = None
        with stream:
            yield stream
    finally:
        if descriptor is not None:
            os.close(descriptor)


def text_encoding(sample):
    # UTF-32 must precede UTF-16 because its LE BOM shares the first two bytes.
    for bom, encoding in (
        (codecs.BOM_UTF32_LE, "utf-32"),
        (codecs.BOM_UTF32_BE, "utf-32"),
        (codecs.BOM_UTF16_LE, "utf-16"),
        (codecs.BOM_UTF16_BE, "utf-16"),
    ):
        if sample.startswith(bom):
            return encoding
    for size in (188, 192, 204):
        for offset in (0, 4):
            if len(sample) > offset + size * 3 and all(
                sample[offset + size * n] == 0x47 for n in range(4)
            ):
                raise ValueError("binary_content:mpeg_transport_stream")
    controls = sum(byte < 32 and byte not in (9, 10, 12, 13) for byte in sample)
    if b"\0" in sample or (sample and controls / len(sample) > 0.02):
        raise ValueError("binary_content:non_text_bytes")
    for encoding in ("utf-8-sig", "gb18030"):
        try:
            codecs.getincrementaldecoder(encoding)().decode(sample, final=False)
            return encoding
        except UnicodeError:
            pass
    return "utf-8-sig"


def document_char_limit():
    value = int(os.environ.get("AI_SPACE_MAX_DOCUMENT_CHARS", DEFAULT_DOCUMENT_CHARS))
    return max(1024, min(value, 64 * 1024 * 1024))


def text_lines(path, replaced=None, truncated=None):
    """Yield bounded line pieces; a newline-free log cannot allocate one huge str."""
    import io

    with open_regular_binary(path) as source:
        sample = source.read(SAMPLE_BYTES)
        encoding = text_encoding(sample)
        source.seek(0)
        size = os.fstat(source.fileno()).st_size
        budget = max(SAMPLE_BYTES, document_char_limit() * 4)
        with io.TextIOWrapper(source, encoding=encoding, errors="replace") as stream:
            line = 1
            while True:
                if size > budget and source.tell() >= budget:
                    if truncated:
                        truncated()
                    break
                value = stream.readline(TEXT_READ_CHARS)
                if not value:
                    break
                if "\ufffd" in value and replaced:
                    replaced()
                yield line, value
                if value.endswith("\n"):
                    line += 1
