"""Pull text out of a matplotlib PDF, including Flate streams."""
from __future__ import annotations

import zlib
from pathlib import Path


def pdf_bytes(path: Path) -> bytes:
    data = path.read_bytes()
    parts = [data]
    cursor = 0
    while True:
        start = data.find(b"stream", cursor)
        if start < 0:
            break
        newline = data.find(b"\n", start)
        if newline < 0:
            break
        end = data.find(b"endstream", newline)
        if end < 0:
            break
        payload = data[newline + 1:end].strip(b"\r\n")
        try:
            parts.append(zlib.decompress(payload))
        except zlib.error:
            pass
        cursor = end + len(b"endstream")
    return b"\n".join(parts)


def has_label(blob: bytes, label: str) -> bool:
    raw = label.encode("ascii")
    utf16 = label.encode("utf-16-be")
    return raw in blob or utf16 in blob
