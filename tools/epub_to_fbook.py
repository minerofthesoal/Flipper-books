#!/usr/bin/env python3
"""Convert an EPUB to the Flipper Books .fbook format.

Usage:
    python3 tools/epub_to_fbook.py book.epub [-o out.fbook] [--no-images]

The resulting .fbook file is a compact, stream-readable binary layout
that the on-device reader understands without any ZIP/HTML parser.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import zipfile
from dataclasses import dataclass, field
from html.parser import HTMLParser
from typing import Dict, List, Optional, Tuple
from xml.etree import ElementTree as ET

try:
    from PIL import Image, ImageOps, ImageFilter, ImageEnhance  # type: ignore
except ImportError:  # pragma: no cover - Pillow is optional
    Image = None  # type: ignore
    ImageOps = None  # type: ignore
    ImageFilter = None  # type: ignore
    ImageEnhance = None  # type: ignore

FBOOK_V1_MAGIC = b"FBOOK\x01"
FBOOK_V2_MAGIC = b"FBOOK\x02"
TITLE_LEN = 64
AUTHOR_LEN = 48
LANGUAGE_LEN = 8
CHAPTER_TITLE_V1 = 32
CHAPTER_TITLE_V2 = 48
IMAGE_MAX_WIDTH = 128
IMAGE_MAX_HEIGHT = 64           # v2 raised from 48
COVER_MAX_WIDTH = 64            # cover thumbnail size for the library view
COVER_MAX_HEIGHT = 64
FBOOK2_FLAG_HAS_COVER = 0x0001
FBOOK2_FLAG_CHAPTER_PAGES = 0x0004
# Must match FBOOK_MAX_CHAPTERS / FBOOK_MAX_IMAGES in src/helpers/book_storage.h.
# If a book exceeds these, the on-device reader silently drops the excess, so we
# truncate here too - otherwise the file pointer desyncs and the reader can
# render garbage or nothing at all.
MAX_CHAPTERS = 128
MAX_IMAGES = 64
V2_HEADER_SIZE = 224
V2_CHAPTER_SIZE = 4 + 4 + CHAPTER_TITLE_V2  # 56
V2_IMAGE_SIZE = 4 + 2 + 2 + 4 + 4 + 1 + 3   # 20

NS = {
    "opf": "http://www.idpf.org/2007/opf",
    "dc": "http://purl.org/dc/elements/1.1/",
    "container": "urn:oasis:names:tc:opendocument:xmlns:container",
    "xhtml": "http://www.w3.org/1999/xhtml",
    "ncx": "http://www.daisy.org/z3986/2005/ncx/",
}

DISPLAY_TRANSLATION = {
    ord("\u2018"): "'",
    ord("\u2019"): "'",
    ord("\u201a"): "'",
    ord("\u201b"): "'",
    ord("\u2032"): "'",
    ord("\u201c"): '"',
    ord("\u201d"): '"',
    ord("\u201e"): '"',
    ord("\u201f"): '"',
    ord("\u2033"): '"',
    ord("\u2010"): "-",
    ord("\u2011"): "-",
    ord("\u2012"): "-",
    ord("\u2013"): "-",
    ord("\u2014"): "-",
    ord("\u2015"): "-",
    ord("\u2212"): "-",
    ord("\u2026"): "...",
    ord("\u00a0"): " ",
}


def _display_text(s: str) -> str:
    """Normalize punctuation to glyphs the Flipper built-in fonts render well."""
    return s.translate(DISPLAY_TRANSLATION)


@dataclass
class ChapterOut:
    offset: int
    title: str


@dataclass
class ChapterOutV2:
    offset: int
    word_count: int
    title: str
    page: int = 0


@dataclass
class ImageOut:
    offset_in_text: int
    width: int
    height: int
    data: bytes
    fmt: int = 0  # 0 = 1bpp, 1 = 2bpp grayscale


class _TextExtractor(HTMLParser):
    """Very small HTML->text converter that keeps paragraph breaks and notes images."""

    SKIP_TAGS = {"script", "style", "head", "nav"}
    # Void elements per HTML spec - they can't have descendants. We do NOT track
    # a skip-depth for them, otherwise an XHTML-style self-closing <meta/> or
    # <link/> in <head> (which has no matching end tag) leaves the parser stuck
    # in skip mode forever and the entire body of the book is silently dropped.
    VOID_SKIP_TAGS = {"meta", "link", "base", "param", "source", "track"}
    BLOCK_TAGS = {
        "p", "div", "section", "article", "li", "ul", "ol",
        "h1", "h2", "h3", "h4", "h5", "h6", "br", "hr", "blockquote",
        "pre",
    }

    def __init__(self, image_hook=None):
        super().__init__(convert_charrefs=True)
        self.chunks: List[str] = []
        self._skip_depth = 0
        self._image_hook = image_hook

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag in self.VOID_SKIP_TAGS:
            return
        if tag in self.SKIP_TAGS:
            self._skip_depth += 1
            return
        if self._skip_depth:
            return
        if tag == "img" and self._image_hook:
            src = dict(attrs).get("src")
            if src:
                marker = self._image_hook(src)
                if marker:
                    self.chunks.append(marker)
                    return
        if tag in self.BLOCK_TAGS:
            self.chunks.append("\n\n")

    def handle_endtag(self, tag):
        tag = tag.lower()
        if tag in self.VOID_SKIP_TAGS:
            return
        if tag in self.SKIP_TAGS:
            if self._skip_depth > 0:
                self._skip_depth -= 1
            return
        if self._skip_depth:
            return
        if tag in self.BLOCK_TAGS:
            self.chunks.append("\n")

    def handle_startendtag(self, tag, attrs):
        tl = tag.lower()
        if tl == "br":
            if not self._skip_depth:
                self.chunks.append("\n")
            return
        if tl in self.VOID_SKIP_TAGS:
            return
        if tl in self.SKIP_TAGS:
            # Self-closing <head/> etc. has no descendants, no skip context.
            return
        # img, hr and friends: run start logic only.
        self.handle_starttag(tag, attrs)

    def handle_data(self, data):
        if self._skip_depth:
            return
        self.chunks.append(data)

    def get_text(self) -> str:
        text = "".join(self.chunks)
        text = re.sub(r"[ \t]+", " ", text)
        text = re.sub(r" *\n *", "\n", text)
        text = re.sub(r"\n{3,}", "\n\n", text)
        return _display_text(text.strip())


def _prepare_for_1bit(im, target_w, target_h):
    """Pre-process a PIL image so the 1bpp quantizer produces something
    legible on the Flipper's tiny screen.

    Steps:
      1. flatten transparency onto a white page (converter EPUBs frequently
         have RGBA cover art with a transparent backdrop; without this PIL
         premultiplies against black and the cover comes out as a blob);
      2. convert to 8-bit grayscale ("L");
      3. autocontrast - stretches the grayscale histogram so the darkest
         pixel maps to 0 and the lightest to 255. Gives the dither room to
         work even on flat scans;
      4. mild sharpening pass with a 3x3 unsharp mask. This is a big
         readability win at thumbnail sizes - small text and outlines pop
         instead of dissolving;
      5. resize with LANCZOS, which preserves detail far better than the
         BILINEAR PIL falls back to when no resample is given;
      6. clamp to the requested dimensions.
    """
    if im.mode in ("RGBA", "LA", "PA"):
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        bg.paste(im, mask=im.split()[-1])
        im = bg.convert("RGB")
    im = im.convert("L")
    if ImageOps is not None:
        im = ImageOps.autocontrast(im, cutoff=2)
    if ImageFilter is not None:
        im = im.filter(ImageFilter.UnsharpMask(radius=1.0, percent=120, threshold=3))
    w, h = im.size
    scale = min(target_w / w, target_h / h, 1.0)
    if scale < 1.0:
        w = max(1, int(w * scale))
        h = max(1, int(h * scale))
        im = im.resize((w, h), Image.LANCZOS if hasattr(Image, "LANCZOS") else Image.BICUBIC)
    return im, w, h


def _pack_1bpp_lsb(mono, w, h) -> bytes:
    """Pack a PIL '1' image into LSB-first row-major bytes (XBM order)."""
    row_bytes = (w + 7) // 8
    out = bytearray(row_bytes * h)
    px = mono.load()
    for y in range(h):
        for x in range(w):
            if px[x, y] == 0:  # black
                out[y * row_bytes + (x >> 3)] |= 1 << (x & 7)
    return bytes(out)


def _dither_to_1bpp(im) -> Tuple[int, int, bytes]:
    """High-quality 1bpp conversion for inline images: autocontrast +
    unsharp mask + LANCZOS resize + Floyd-Steinberg dither. The result is
    written in XBM byte order (LSB first within each byte) so it goes
    straight into canvas_draw_xbm at runtime."""
    im, w, h = _prepare_for_1bit(im, IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT)
    # PIL's "1" conversion uses Floyd-Steinberg dither by default, which
    # works well on photographic content. For line-art (most diagrams in
    # technical books) it can produce noisy speckle, but the alternative
    # (no dither) loses gradients - dither is the better default.
    mono = im.convert("1")
    return w, h, _pack_1bpp_lsb(mono, w, h)


def _find_opf(zf: zipfile.ZipFile) -> str:
    try:
        with zf.open("META-INF/container.xml") as f:
            root = ET.parse(f).getroot()
    except KeyError as exc:
        raise ValueError("Not a valid EPUB: missing META-INF/container.xml") from exc
    rootfile = root.find(".//container:rootfile", NS)
    if rootfile is None:
        raise ValueError("EPUB container.xml missing rootfile")
    return rootfile.attrib["full-path"]


def _load_spine(zf: zipfile.ZipFile, opf_path: str):
    with zf.open(opf_path) as f:
        root = ET.parse(f).getroot()

    title = _display_text((root.findtext(".//dc:title", default="", namespaces=NS) or "").strip())
    author = _display_text((root.findtext(".//dc:creator", default="", namespaces=NS) or "").strip())
    language = (root.findtext(".//dc:language", default="", namespaces=NS) or "").strip()

    manifest = {}
    for item in root.findall(".//opf:manifest/opf:item", NS):
        manifest[item.attrib["id"]] = (
            item.attrib["href"],
            item.attrib.get("media-type", ""),
            item.attrib.get("properties", ""),
        )

    spine = root.find(".//opf:spine", NS)
    spine_ids = [
        itemref.attrib["idref"]
        for itemref in root.findall(".//opf:spine/opf:itemref", NS)
    ]

    # Cover detection: prefer the EPUB3 manifest "cover-image" property,
    # then EPUB2 <meta name="cover" content="..."/>, then a manifest id of
    # "cover" or any item whose href looks like a cover image.
    cover_ref = None
    for item_id, (href, media, props) in manifest.items():
        if "cover-image" in (props or ""):
            cover_ref = href
            break
    if cover_ref is None:
        for meta in root.findall(".//opf:metadata/opf:meta", NS):
            if meta.attrib.get("name", "").lower() == "cover":
                ref_id = meta.attrib.get("content")
                if ref_id and ref_id in manifest:
                    cover_ref = manifest[ref_id][0]
                    break
    if cover_ref is None:
        for cand in ("cover", "cover-image", "coverimage"):
            if cand in manifest:
                cover_ref = manifest[cand][0]
                break
    if cover_ref is None:
        for href, media, _ in manifest.values():
            if media.startswith("image/") and "cover" in href.lower():
                cover_ref = href
                break

    base = os.path.dirname(opf_path)

    def resolve(href: str) -> str:
        return os.path.normpath(os.path.join(base, href)).replace("\\", "/")

    def resolve_doc_ref(href: str) -> str:
        return resolve(href.split("#", 1)[0])

    def read_toc_titles() -> Dict[str, str]:
        toc_href = None
        if spine is not None:
            toc_id = spine.attrib.get("toc")
            if toc_id and toc_id in manifest:
                toc_href = manifest[toc_id][0]
        if toc_href is None:
            for href, media, props in manifest.values():
                if media == "application/x-dtbncx+xml" or "nav" in (props or "").split():
                    toc_href = href
                    break
        if toc_href is None:
            return {}

        toc_path = resolve(toc_href)
        try:
            toc_root = ET.fromstring(zf.read(toc_path))
        except Exception:
            return {}

        titles: Dict[str, str] = {}
        for nav_point in toc_root.findall(".//ncx:navPoint", NS):
            label = nav_point.findtext(".//ncx:navLabel/ncx:text", default="", namespaces=NS)
            content = nav_point.find(".//ncx:content", NS)
            if not label or content is None:
                continue
            src = content.attrib.get("src", "")
            if not src:
                continue
            doc = resolve_doc_ref(src)
            titles.setdefault(doc, _display_text(re.sub(r"\s+", " ", label).strip()))
        if titles:
            return titles

        # EPUB3 navigation documents are XHTML. Their TOC is usually an
        # <nav epub:type="toc"> with nested <a href="...">Title</a> entries.
        for elem in toc_root.findall(".//xhtml:a", NS):
            href = elem.attrib.get("href", "")
            label = "".join(elem.itertext())
            if href and label:
                doc = resolve_doc_ref(href)
                titles.setdefault(doc, _display_text(re.sub(r"\s+", " ", label).strip()))
        return titles

    documents = []
    for idref in spine_ids:
        entry = manifest.get(idref)
        if not entry:
            continue
        href, media, _ = entry
        if "html" not in media and not href.lower().endswith((".html", ".xhtml", ".htm")):
            continue
        documents.append(resolve(href))

    cover_path = resolve(cover_ref) if cover_ref else None
    return title, author, language, manifest, base, documents, cover_path, read_toc_titles()


def _extract_title_from_doc(raw_html: str) -> Optional[str]:
    m = re.search(r"<h[12][^>]*>(.*?)</h[12]>", raw_html, re.IGNORECASE | re.DOTALL)
    if m:
        text = re.sub(r"<[^>]+>", "", m.group(1)).strip()
        text = re.sub(r"\s+", " ", text)
        if text:
            return _display_text(text)[:CHAPTER_TITLE_V2 - 1]
    return None


def _word_count(s: str) -> int:
    """Cheap word counter used for progress estimates and chapter metadata."""
    return len(re.findall(r"\b\w+\b", s, flags=re.UNICODE))


def _compute_default_reader_page_end(
    text_buf: bytearray,
    page_offset: int,
    images: List[ImageOut],
) -> int:
    """Mirror the reader's default Small-text pagination for stored TOC pages."""
    cache_len = min(511, max(0, len(text_buf) - page_offset))
    cache = text_buf[page_offset:page_offset + cache_len]

    line_height = 9
    max_chars_per_line = 26
    avail_h = 64 - 3  # default settings show the progress bar

    reserve = 8  # default settings show the page number overlay
    for img in images:
        if img.offset_in_text < page_offset:
            continue
        if img.offset_in_text < page_offset + cache_len:
            h = min(img.height, 48)
            reserve = max(reserve, h + 2 if h + 2 < avail_h else h)
        break

    text_h = avail_h - reserve if avail_h > reserve else line_height
    max_lines = max(1, text_h // line_height)

    i = 0
    lines = 0
    last_break = 0
    while i < cache_len and lines < max_lines:
        line_start = i
        col = 0
        while i < cache_len and cache[i] != 0x0A and col < max_chars_per_line:
            i += 1
            col += 1
        if i < cache_len and cache[i] == 0x0A:
            i += 1
        elif col == max_chars_per_line and i < cache_len:
            j = i
            while j > line_start and cache[j] != 0x20:
                j -= 1
            if j > line_start:
                i = j + 1
        last_break = i
        lines += 1

    if last_break == 0:
        last_break = cache_len
    return min(len(text_buf), page_offset + last_break)


def _assign_default_chapter_pages(
    chapters: List[ChapterOutV2],
    images: List[ImageOut],
    text_buf: bytearray,
) -> None:
    """Precompute TOC page numbers so the device does not scan the book on open."""
    page_offset = 0
    page = 0
    next_chapter = 0

    while next_chapter < len(chapters):
        target = min(chapters[next_chapter].offset, len(text_buf))
        if target <= page_offset:
            chapters[next_chapter].page = page
            next_chapter += 1
            continue

        next_offset = _compute_default_reader_page_end(text_buf, page_offset, images)
        if next_offset <= page_offset:
            next_offset = min(len(text_buf), page_offset + 1)

        if target < next_offset:
            chapters[next_chapter].page = page
            next_chapter += 1
            continue

        page_offset = next_offset
        page += 1
        if page_offset >= len(text_buf):
            while next_chapter < len(chapters):
                chapters[next_chapter].page = page
                next_chapter += 1
            break


def _render_cover_1bpp(im, max_w=COVER_MAX_WIDTH, max_h=COVER_MAX_HEIGHT) -> Tuple[int, int, bytes]:
    """Convert a PIL image to a tightly-packed LSB-first 1bpp cover thumbnail.

    Uses the same autocontrast + unsharp + LANCZOS pipeline as the inline
    image converter, so cover lettering stays legible at the 64x64 size the
    fbook2 format reserves for it. The on-device library view downsamples
    further to 16x16, so feeding it a clean 64x64 here is what gives the
    final thumbnail its detail.
    """
    im, w, h = _prepare_for_1bit(im, max_w, max_h)
    mono = im.convert("1")
    return w, h, _pack_1bpp_lsb(mono, w, h)


def _build_book_data(zf, opf_path, include_images):
    """Walk the spine, extract text, collect images. Returns the per-book
    in-memory representation used by both the v1 and v2 writers."""
    title, author, language, manifest, base_dir, documents, cover_path, toc_titles = _load_spine(
        zf, opf_path)

    text_buf = bytearray()
    chapters: List[ChapterOutV2] = []
    images: List[ImageOut] = []
    image_cache: dict[str, int] = {}

    placeholder_re = re.compile(r"\x01IMG\x01(\d+)\x01")

    for doc_path in documents:
        try:
            raw = zf.read(doc_path).decode("utf-8", errors="replace")
        except KeyError:
            continue
        chap_title = toc_titles.get(doc_path) or _extract_title_from_doc(raw) or os.path.basename(doc_path)
        include_chapter = bool(chap_title) and (not toc_titles or doc_path in toc_titles)

        def image_hook(src: str, _doc_path=doc_path) -> Optional[str]:
            if not include_images or Image is None:
                return None
            doc_dir = os.path.dirname(_doc_path)
            full = os.path.normpath(os.path.join(doc_dir, src)).replace("\\", "/")
            if full in image_cache:
                idx = image_cache[full]
            else:
                if len(images) >= MAX_IMAGES:
                    return None
                try:
                    data = zf.read(full)
                except KeyError:
                    return None
                try:
                    with Image.open(__import__("io").BytesIO(data)) as im:
                        w, h, packed = _dither_to_1bpp(im)
                except Exception:
                    return None
                idx = len(images)
                images.append(
                    ImageOut(offset_in_text=0, width=w, height=h, data=packed, fmt=0))
                image_cache[full] = idx
            return f"\x01IMG\x01{idx}\x01"

        parser = _TextExtractor(image_hook=image_hook)
        parser.feed(raw)
        parser.close()
        text = parser.get_text()

        chapter_start = len(text_buf)
        chap_words = _word_count(text)
        if include_chapter and len(chapters) < MAX_CHAPTERS:
            chapters.append(ChapterOutV2(offset=chapter_start,
                                         word_count=chap_words,
                                         title=chap_title))

        pos = 0
        for m in placeholder_re.finditer(text):
            text_buf.extend(text[pos:m.start()].encode("utf-8", errors="replace"))
            idx = int(m.group(1))
            images[idx].offset_in_text = len(text_buf)
            pos = m.end()
        text_buf.extend(text[pos:].encode("utf-8", errors="replace"))
        text_buf.extend(b"\n\n")

    _assign_default_chapter_pages(chapters, images, text_buf)

    cover_w = cover_h = 0
    cover_data = b""
    if cover_path and Image is not None:
        try:
            raw = zf.read(cover_path)
            with Image.open(__import__("io").BytesIO(raw)) as im:
                cover_w, cover_h, cover_data = _render_cover_1bpp(im)
        except Exception:
            cover_w = cover_h = 0
            cover_data = b""

    return {
        "title": title,
        "author": author,
        "language": language,
        "text_buf": text_buf,
        "chapters": chapters,
        "images": images,
        "cover_w": cover_w,
        "cover_h": cover_h,
        "cover_data": cover_data,
    }


def _write_v1(book, out_path):
    """Write a v1 (.fbook) file. Kept as a fallback for older readers."""
    chapters = book["chapters"]
    images = book["images"]
    text_buf = book["text_buf"]

    header_size = (
        len(FBOOK_V1_MAGIC) + 2 + TITLE_LEN + AUTHOR_LEN + 4 + 4 + 2 + 2
    )
    chapter_table_size = len(chapters) * (4 + CHAPTER_TITLE_V1)
    image_table_size = len(images) * (4 + 2 + 2 + 4)
    text_offset = header_size + chapter_table_size + image_table_size
    text_len = len(text_buf)
    cursor = text_offset + text_len
    for img in images:
        img.data_offset = cursor  # type: ignore[attr-defined]
        cursor += len(img.data)

    with open(out_path, "wb") as f:
        f.write(FBOOK_V1_MAGIC)
        f.write(struct.pack("<H", 1))
        f.write(book["title"].encode("utf-8")[:TITLE_LEN - 1].ljust(TITLE_LEN, b"\x00"))
        f.write(book["author"].encode("utf-8")[:AUTHOR_LEN - 1].ljust(AUTHOR_LEN, b"\x00"))
        f.write(struct.pack("<I", text_offset))
        f.write(struct.pack("<I", text_len))
        f.write(struct.pack("<H", len(chapters)))
        f.write(struct.pack("<H", len(images)))
        for ch in chapters:
            f.write(struct.pack("<I", ch.offset))
            f.write(ch.title.encode("utf-8")[:CHAPTER_TITLE_V1 - 1].ljust(CHAPTER_TITLE_V1, b"\x00"))
        for img in images:
            f.write(struct.pack("<I", img.offset_in_text))
            f.write(struct.pack("<H", img.width))
            f.write(struct.pack("<H", img.height))
            f.write(struct.pack("<I", img.data_offset))  # type: ignore[attr-defined]
        f.write(bytes(text_buf))
        for img in images:
            f.write(img.data)


def _write_v2(book, out_path):
    """Write a v2 (.fbook2) file with cover thumbnail and per-chapter word
    counts. The reader auto-detects v1 vs v2 from the magic prefix."""
    chapters = book["chapters"]
    images = book["images"]
    text_buf = book["text_buf"]
    cover_w = book["cover_w"]
    cover_h = book["cover_h"]
    cover_data = book["cover_data"]

    chapter_table_size = len(chapters) * V2_CHAPTER_SIZE
    image_table_size = len(images) * V2_IMAGE_SIZE
    chapter_page_table_size = len(chapters) * 4
    text_offset = V2_HEADER_SIZE + chapter_table_size + image_table_size + chapter_page_table_size
    text_len = len(text_buf)

    cursor = text_offset + text_len
    cover_offset = 0
    if cover_data:
        cover_offset = cursor
        cursor += len(cover_data)
    for img in images:
        img.data_offset = cursor  # type: ignore[attr-defined]
        cursor += len(img.data)

    word_count = sum(c.word_count for c in chapters)
    flags = 0
    if cover_data:
        flags |= FBOOK2_FLAG_HAS_COVER
    if chapters:
        flags |= FBOOK2_FLAG_CHAPTER_PAGES

    with open(out_path, "wb") as f:
        f.write(FBOOK_V2_MAGIC)
        f.write(struct.pack("<H", 2))   # version
        f.write(struct.pack("<H", flags))
        f.write(book["title"].encode("utf-8")[:TITLE_LEN - 1].ljust(TITLE_LEN, b"\x00"))
        f.write(book["author"].encode("utf-8")[:AUTHOR_LEN - 1].ljust(AUTHOR_LEN, b"\x00"))
        f.write(book["language"].encode("utf-8")[:LANGUAGE_LEN - 1].ljust(LANGUAGE_LEN, b"\x00"))
        f.write(struct.pack("<I", text_offset))
        f.write(struct.pack("<I", text_len))
        f.write(struct.pack("<I", word_count))
        f.write(struct.pack("<H", len(chapters)))
        f.write(struct.pack("<H", len(images)))
        f.write(struct.pack("<I", cover_offset))
        f.write(struct.pack("<H", cover_w))
        f.write(struct.pack("<H", cover_h))
        f.write(struct.pack("<I", len(cover_data)))
        f.write(struct.pack("<B", 0))  # cover_format = 1bpp
        f.write(struct.pack("<B", 0))  # default image_format = 1bpp
        # Pad header to V2_HEADER_SIZE.
        consumed = (
            len(FBOOK_V2_MAGIC) + 2 + 2 + TITLE_LEN + AUTHOR_LEN + LANGUAGE_LEN +
            4 + 4 + 4 + 2 + 2 + 4 + 2 + 2 + 4 + 1 + 1
        )
        f.write(b"\x00" * (V2_HEADER_SIZE - consumed))

        for ch in chapters:
            f.write(struct.pack("<I", ch.offset))
            f.write(struct.pack("<I", ch.word_count))
            f.write(ch.title.encode("utf-8")[:CHAPTER_TITLE_V2 - 1].ljust(CHAPTER_TITLE_V2, b"\x00"))
        for img in images:
            f.write(struct.pack("<I", img.offset_in_text))
            f.write(struct.pack("<H", img.width))
            f.write(struct.pack("<H", img.height))
            f.write(struct.pack("<I", img.data_offset))  # type: ignore[attr-defined]
            f.write(struct.pack("<I", len(img.data)))
            f.write(struct.pack("<B", img.fmt))
            f.write(b"\x00" * 3)
        for ch in chapters:
            f.write(struct.pack("<I", ch.page))
        f.write(bytes(text_buf))
        if cover_data:
            f.write(cover_data)
        for img in images:
            f.write(img.data)


def convert(
    epub_path: str,
    out_path: Optional[str] = None,
    include_images: bool = True,
    fmt_version: int = 2,
) -> str:
    if out_path is None:
        base = os.path.splitext(os.path.basename(epub_path))[0]
        out_path = base + ".fbook"

    with zipfile.ZipFile(epub_path) as zf:
        opf_path = _find_opf(zf)
        book = _build_book_data(zf, opf_path, include_images)

        if fmt_version == 1:
            _write_v1(book, out_path)
        else:
            _write_v2(book, out_path)
    return out_path


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Convert EPUB to Flipper .fbook / .fbook2")
    parser.add_argument("epub", help="Path to input .epub file")
    parser.add_argument("-o", "--output", help="Output .fbook path")
    parser.add_argument("--no-images", action="store_true", help="Strip images")
    parser.add_argument("--v1", action="store_true",
                        help="Emit legacy v1 format (no cover, smaller images, no chapter word counts)")
    args = parser.parse_args(argv)

    fmt = 1 if args.v1 else 2
    out = convert(args.epub, args.output,
                  include_images=not args.no_images,
                  fmt_version=fmt)
    print(f"Wrote {out} (v{fmt})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
