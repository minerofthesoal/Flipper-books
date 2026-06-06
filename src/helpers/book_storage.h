#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <storage/storage.h>

#define FBOOK_MAGIC      "FBOOK\x01"
#define FBOOK2_MAGIC     "FBOOK\x02"
#define FBOOK_MAGIC_LEN  6
#define FBOOK_MAX_TITLE  64
#define FBOOK_MAX_AUTHOR 48
#define FBOOK_MAX_LANG   8
#define FBOOK_MAX_CHAPTERS 128
#define FBOOK_MAX_IMAGES 64
#define FBOOK_CHAPTER_TITLE_V1 32
#define FBOOK_CHAPTER_TITLE_V2 48
#define FBOOK2_FLAG_HAS_COVER 0x0001u
#define FBOOK2_FLAG_CHAPTER_PAGES 0x0004u

/*
 * .fbook (v1) on-disk format (little endian):
 *   char     magic[6]        = "FBOOK\x01"
 *   uint16   version         = 1
 *   char     title[64]
 *   char     author[48]
 *   uint32   text_offset
 *   uint32   text_length
 *   uint16   chapter_count
 *   uint16   image_count
 *   ChapterV1 chapters[chapter_count]    // 36 bytes each (uint32 offset + char title[32])
 *   ImageV1  images[image_count]         // 12 bytes each (u32+u16+u16+u32)
 *   byte     text[text_length]           // UTF-8, normalized
 *   byte     image_data[...]             // 1bpp tiles
 *
 * .fbook2 on-disk format (little endian):
 *   char     magic[6]        = "FBOOK\x02"
 *   uint16   version         = 2
 *   uint16   flags           // bit 0: has-cover; bit 1: 2bpp images; bit 2: chapter page table
 *   char     title[64]
 *   char     author[48]
 *   char     language[8]
 *   uint32   text_offset
 *   uint32   text_length
 *   uint32   word_count
 *   uint16   chapter_count
 *   uint16   image_count
 *   uint32   cover_offset    // 0 if no cover; else absolute file offset
 *   uint16   cover_w
 *   uint16   cover_h
 *   uint32   cover_data_len
 *   uint8    cover_format    // 0 = 1bpp, 1 = 2bpp grayscale
 *   uint8    image_format    // 0 = 1bpp, 1 = 2bpp grayscale
 *   uint8    reserved[14]    // pad header to 224 bytes total
 *   ChapterV2 chapters[chapter_count]   // 56 bytes each (u32 offset + u32 word_count + char title[48])
 *   ImageV2  images[image_count]        // 20 bytes each (u32 offset_in_text + u16 w + u16 h + u32 data_offset + u32 data_len + u8 format + u8 reserved[3])
 *   uint32   chapter_pages[chapter_count] // optional, when flags bit 2 is set; zero-based
 *   byte     text[text_length]
 *   byte     cover_data[cover_data_len]
 *   byte     image_data[...]
 *
 * Pixel format (1bpp): row-major, LSB-first within each byte (bit 0 = leftmost).
 * Pixel format (2bpp grayscale): row-major, 4 pixels per byte, MSB pair first; values 0..3 (0=lightest, 3=darkest).
 */

typedef struct {
    uint32_t offset;
    uint32_t word_count; // v2 only; 0 for v1
    uint32_t page;       // optional v2 chapter page table; 0 for older files
    char     title[FBOOK_CHAPTER_TITLE_V2];
} FBookChapter;

typedef struct {
    uint32_t offset_in_text;
    uint16_t w;
    uint16_t h;
    uint32_t data_offset;
    uint32_t data_len;
    uint8_t  format;     // 0 = 1bpp, 1 = 2bpp; v1 always 0
} FBookImage;

typedef struct {
    char path[256];
    uint8_t version;     // 1 or 2

    char title[FBOOK_MAX_TITLE];
    char author[FBOOK_MAX_AUTHOR];
    char language[FBOOK_MAX_LANG];
    uint16_t flags;

    uint32_t text_offset;
    uint32_t text_length;
    uint32_t word_count; // v2; 0 for v1
    uint16_t chapter_count;
    uint16_t image_count;

    // Cover (v2 only). cover_w == 0 means no cover.
    uint32_t cover_offset;
    uint16_t cover_w;
    uint16_t cover_h;
    uint32_t cover_data_len;
    uint8_t  cover_format;
    uint8_t  image_format; // default for inline images

    FBookChapter chapters[FBOOK_MAX_CHAPTERS];
    FBookImage   images[FBOOK_MAX_IMAGES];

    File* file;      // open handle for streamed text access
    Storage* storage;
} FBook;

FBook* fbook_alloc(void);
void fbook_free(FBook* b);

/** Open a book file. Accepts .fbook (v1 or v2) directly, or converts .epub/.txt to a cached .fbook. */
bool fbook_open(FBook* b, const char* path);

void fbook_close(FBook* b);

/** Read a chunk of decoded text from a given byte offset. Returns bytes read. */
uint32_t fbook_read(FBook* b, uint32_t offset, char* out, uint32_t max_bytes);

/** Return the image bitmap data at images[i]; caller frees. Returns NULL on error.
 *  Output is the on-disk packed format (1bpp LSB-first XBM-style, or 2bpp). */
uint8_t* fbook_load_image(FBook* b, uint16_t index, uint16_t* w, uint16_t* h, uint8_t* format);

/** Return the cover bitmap. Caller frees. Returns NULL if absent. */
uint8_t* fbook_load_cover(FBook* b, uint16_t* w, uint16_t* h, uint8_t* format);

/** Find chapter index that contains a given text offset. */
uint16_t fbook_find_chapter(const FBook* b, uint32_t offset);

/** Find next image at or after a given text offset; returns image index or UINT16_MAX. */
uint16_t fbook_next_image(const FBook* b, uint32_t offset);

/** Search plain text for a needle starting at offset. Returns match offset or UINT32_MAX. */
uint32_t fbook_search(FBook* b, uint32_t start_offset, const char* needle);

/** Convert .txt file to .fbook (v1) in cache dir. Writes output path into out_path. */
bool fbook_import_txt(const char* txt_path, char* out_path, size_t out_len);

/** Best-effort EPUB import - currently looks for a converter-produced cache file. */
bool fbook_import_epub(const char* epub_path, char* out_path, size_t out_len);

/** Enumerate candidate books in the library dir. Returns count copied into paths. */
uint16_t fbook_scan_library(char paths[][256], uint16_t max_paths);

/** Delete a book file plus its cached .fbook (if any) and its progress sidecar.
 *  Returns true if at least the primary file was removed. */
bool fbook_delete(const char* book_path);

/** Quick header peek without keeping the file open - fills title/author and cover meta only. */
bool fbook_peek(const char* path,
                char* title, size_t title_len,
                char* author, size_t author_len,
                uint32_t* text_length,
                uint32_t* word_count);

/** Load a cover thumbnail directly from a .fbook2 path without a full open.
 *  Returns NULL if the file is v1 or has no cover. Caller frees. */
uint8_t* fbook_peek_cover(const char* path, uint16_t* w, uint16_t* h, uint8_t* format);
