"""Bounded visible-text RTF reader; no pictures, objects or rendering engine."""
import codecs
import re

TOKEN = re.compile(
    rb"\\([a-zA-Z]+)(-?\d+)? ?|\\'([0-9a-fA-F]{2})|\\([^a-zA-Z])|([{}])|([^\\{}]+)", re.S
)
DESTINATIONS = {
    b"fonttbl",
    b"colortbl",
    b"stylesheet",
    b"info",
    b"pict",
    b"object",
    b"filetbl",
    b"listtable",
    b"listoverridetable",
    b"xmlnstbl",
    b"datastore",
    b"themedata",
    b"generator",
    b"fldinst",
}
SYMBOLS = {
    b"par": "\n",
    b"line": "\n",
    b"tab": "\t",
    b"emdash": "—",
    b"endash": "–",
    b"bullet": "•",
    b"lquote": "‘",
    b"rquote": "’",
    b"ldblquote": "“",
    b"rdblquote": "”",
}


def visible_text(data):
    stack = []
    skip, fallback, codec = False, 1, "cp1252"
    skip_characters = 0
    output = []
    pending = bytearray()

    def flush():
        if pending:
            output.append(bytes(pending).decode(codec, errors="replace"))
            pending.clear()

    at = 0
    while at < len(data):
        token = TOKEN.match(data, at)
        if not token:
            at += 1
            continue
        at = token.end()
        word, number, hexadecimal, escaped, brace, plain = token.groups()
        if brace:
            flush()
            if brace == b"{":
                if len(stack) >= 256:
                    raise ValueError("rtf_nesting_limit")
                stack.append((skip, fallback, codec))
            elif stack:
                skip, fallback, codec = stack.pop()
        elif word:
            if word == b"bin":
                amount = max(0, int(number or b"0"))
                at = min(len(data), at + amount)
            elif word in DESTINATIONS:
                flush()
                skip = True
            elif word == b"uc":
                fallback = max(0, min(16, int(number or b"1")))
            elif word == b"ansicpg":
                flush()
                candidate = "cp" + (number or b"1252").decode()
                codecs.lookup(candidate)
                codec = candidate
            elif not skip and word == b"u":
                flush()
                output.append(chr(int(number or b"0") & 0xFFFF))
                skip_characters = fallback
            elif not skip and word in SYMBOLS:
                flush()
                output.append(SYMBOLS[word])
        elif escaped == b"*":
            flush()
            skip = True
        elif not skip:
            value = bytes.fromhex(hexadecimal.decode()) if hexadecimal else escaped or plain or b""
            if escaped == b"~":
                value = b" "
            # Literal CR/LF is RTF source formatting; \par/\line emits newlines.
            if plain:
                value = value.replace(b"\r", b"").replace(b"\n", b"")
            if skip_characters:
                count = min(len(value), skip_characters)
                value = value[count:]
                skip_characters -= count
            pending.extend(value)
    flush()
    text = "".join(output)
    return text.encode("utf-16", "surrogatepass").decode("utf-16", errors="replace")
