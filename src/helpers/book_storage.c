#include "book_storage.h"
#include "../books_app.h"

#include <furi.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FBOOK_V1_HEADER_SIZE  (FBOOK_MAGIC_LEN + 2 + FBOOK_MAX_TITLE + FBOOK_MAX_AUTHOR + 4 + 4 + 2 + 2)
#define FBOOK_V2_HEADER_SIZE  224

#define FBOOK_V1_CHAPTER_SIZE 36
#define FBOOK_V1_IMAGE_SIZE   12
#define FBOOK_V2_CHAPTER_SIZE (4 + 4 + FBOOK_CHAPTER_TITLE_V2)  // 56
#define FBOOK_V2_IMAGE_SIZE   (4 + 2 + 2 + 4 + 4 + 1 + 3)        // 20

FBook* fbook_alloc(void) {
    FBook* b = malloc(sizeof(FBook));
    if(!b) return NULL;
    memset(b, 0, sizeof(*b));
    return b;
}

void fbook_free(FBook* b) {
    if(!b) return;
    fbook_close(b);
    free(b);
}

void fbook_close(FBook* b) {
    if(b->file) {
        storage_file_close(b->file);
        storage_file_free(b->file);
        b->file = NULL;
    }
    if(b->storage) {
        furi_record_close(RECORD_STORAGE);
        b->storage = NULL;
    }
}

static uint16_t read_u16(File* f) {
    uint8_t buf[2] = {0};
    storage_file_read(f, buf, 2);
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t read_u32(File* f) {
    uint8_t buf[4] = {0};
    storage_file_read(f, buf, 4);
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static uint8_t read_u8(File* f) {
    uint8_t b = 0;
    storage_file_read(f, &b, 1);
    return b;
}

static bool parse_header_v1(FBook* b) {
    /* Magic + version already consumed. */
    if(storage_file_read(b->file, b->title, FBOOK_MAX_TITLE) != FBOOK_MAX_TITLE) return false;
    if(storage_file_read(b->file, b->author, FBOOK_MAX_AUTHOR) != FBOOK_MAX_AUTHOR) return false;
    b->title[FBOOK_MAX_TITLE - 1] = '\0';
    b->author[FBOOK_MAX_AUTHOR - 1] = '\0';
    b->language[0] = '\0';

    b->text_offset = read_u32(b->file);
    b->text_length = read_u32(b->file);
    b->chapter_count = read_u16(b->file);
    b->image_count = read_u16(b->file);
    b->word_count = 0;
    b->cover_w = 0;
    b->cover_h = 0;
    b->cover_offset = 0;
    b->cover_data_len = 0;
    b->cover_format = 0;
    b->image_format = 0;
    b->flags = 0;

    if(b->chapter_count > FBOOK_MAX_CHAPTERS) {
        /* Don't silently drop: skip the trailing entries by seeking past them
         * after we've read what we can keep. */
        uint16_t over = b->chapter_count - FBOOK_MAX_CHAPTERS;
        b->chapter_count = FBOOK_MAX_CHAPTERS;
        for(uint16_t i = 0; i < FBOOK_MAX_CHAPTERS; ++i) {
            b->chapters[i].offset = read_u32(b->file);
            b->chapters[i].word_count = 0;
            b->chapters[i].page = 0;
            uint8_t tmp[FBOOK_CHAPTER_TITLE_V1];
            if(storage_file_read(b->file, tmp, FBOOK_CHAPTER_TITLE_V1) != FBOOK_CHAPTER_TITLE_V1) return false;
            memset(b->chapters[i].title, 0, sizeof(b->chapters[i].title));
            memcpy(b->chapters[i].title, tmp, FBOOK_CHAPTER_TITLE_V1 - 1);
        }
        if(!storage_file_seek(
               b->file, (uint32_t)over * FBOOK_V1_CHAPTER_SIZE, false)) {
            return false;
        }
    } else {
        for(uint16_t i = 0; i < b->chapter_count; ++i) {
            b->chapters[i].offset = read_u32(b->file);
            b->chapters[i].word_count = 0;
            b->chapters[i].page = 0;
            uint8_t tmp[FBOOK_CHAPTER_TITLE_V1];
            if(storage_file_read(b->file, tmp, FBOOK_CHAPTER_TITLE_V1) != FBOOK_CHAPTER_TITLE_V1) return false;
            memset(b->chapters[i].title, 0, sizeof(b->chapters[i].title));
            memcpy(b->chapters[i].title, tmp, FBOOK_CHAPTER_TITLE_V1 - 1);
        }
    }

    if(b->image_count > FBOOK_MAX_IMAGES) {
        uint16_t over = b->image_count - FBOOK_MAX_IMAGES;
        b->image_count = FBOOK_MAX_IMAGES;
        for(uint16_t i = 0; i < FBOOK_MAX_IMAGES; ++i) {
            b->images[i].offset_in_text = read_u32(b->file);
            b->images[i].w = read_u16(b->file);
            b->images[i].h = read_u16(b->file);
            b->images[i].data_offset = read_u32(b->file);
            b->images[i].data_len =
                ((uint32_t)b->images[i].w * b->images[i].h + 7) / 8;
            b->images[i].format = 0;
        }
        if(!storage_file_seek(
               b->file, (uint32_t)over * FBOOK_V1_IMAGE_SIZE, false)) {
            return false;
        }
    } else {
        for(uint16_t i = 0; i < b->image_count; ++i) {
            b->images[i].offset_in_text = read_u32(b->file);
            b->images[i].w = read_u16(b->file);
            b->images[i].h = read_u16(b->file);
            b->images[i].data_offset = read_u32(b->file);
            b->images[i].data_len =
                ((uint32_t)b->images[i].w * b->images[i].h + 7) / 8;
            b->images[i].format = 0;
        }
    }
    b->version = 1;
    return true;
}

static bool parse_header_v2(FBook* b) {
    /* Magic + version already consumed. We're at file offset 8. */
    b->flags = read_u16(b->file);
    if(storage_file_read(b->file, b->title, FBOOK_MAX_TITLE) != FBOOK_MAX_TITLE) return false;
    if(storage_file_read(b->file, b->author, FBOOK_MAX_AUTHOR) != FBOOK_MAX_AUTHOR) return false;
    if(storage_file_read(b->file, b->language, FBOOK_MAX_LANG) != FBOOK_MAX_LANG) return false;
    b->title[FBOOK_MAX_TITLE - 1] = '\0';
    b->author[FBOOK_MAX_AUTHOR - 1] = '\0';
    b->language[FBOOK_MAX_LANG - 1] = '\0';

    b->text_offset = read_u32(b->file);
    b->text_length = read_u32(b->file);
    b->word_count = read_u32(b->file);
    b->chapter_count = read_u16(b->file);
    b->image_count = read_u16(b->file);
    b->cover_offset = read_u32(b->file);
    b->cover_w = read_u16(b->file);
    b->cover_h = read_u16(b->file);
    b->cover_data_len = read_u32(b->file);
    b->cover_format = read_u8(b->file);
    b->image_format = read_u8(b->file);

    /* Skip reserved bytes to reach the chapter table (header is fixed-size). */
    uint32_t consumed_after_magic =
        2 /*flags*/ + FBOOK_MAX_TITLE + FBOOK_MAX_AUTHOR + FBOOK_MAX_LANG +
        4 + 4 + 4 + 2 + 2 + 4 + 2 + 2 + 4 + 1 + 1;
    uint32_t header_consumed = FBOOK_MAGIC_LEN + 2 + consumed_after_magic;
    if(header_consumed < FBOOK_V2_HEADER_SIZE) {
        if(!storage_file_seek(b->file, FBOOK_V2_HEADER_SIZE - header_consumed, false)) return false;
    }

    uint16_t saved_chapter_count = b->chapter_count;
    if(b->chapter_count > FBOOK_MAX_CHAPTERS) b->chapter_count = FBOOK_MAX_CHAPTERS;
    for(uint16_t i = 0; i < b->chapter_count; ++i) {
        b->chapters[i].offset = read_u32(b->file);
        b->chapters[i].word_count = read_u32(b->file);
        b->chapters[i].page = 0;
        if(storage_file_read(b->file, b->chapters[i].title, FBOOK_CHAPTER_TITLE_V2) !=
           FBOOK_CHAPTER_TITLE_V2)
            return false;
        b->chapters[i].title[FBOOK_CHAPTER_TITLE_V2 - 1] = '\0';
    }
    if(saved_chapter_count > b->chapter_count) {
        uint16_t over = saved_chapter_count - b->chapter_count;
        if(!storage_file_seek(
               b->file, (uint32_t)over * FBOOK_V2_CHAPTER_SIZE, false))
            return false;
    }

    uint16_t saved_image_count = b->image_count;
    if(b->image_count > FBOOK_MAX_IMAGES) b->image_count = FBOOK_MAX_IMAGES;
    for(uint16_t i = 0; i < b->image_count; ++i) {
        b->images[i].offset_in_text = read_u32(b->file);
        b->images[i].w = read_u16(b->file);
        b->images[i].h = read_u16(b->file);
        b->images[i].data_offset = read_u32(b->file);
        b->images[i].data_len = read_u32(b->file);
        b->images[i].format = read_u8(b->file);
        uint8_t r[3];
        storage_file_read(b->file, r, 3);
        (void)r;
    }
    if(saved_image_count > b->image_count) {
        uint16_t over = saved_image_count - b->image_count;
        if(!storage_file_seek(
               b->file, (uint32_t)over * FBOOK_V2_IMAGE_SIZE, false))
            return false;
    }
    if(b->flags & FBOOK2_FLAG_CHAPTER_PAGES) {
        for(uint16_t i = 0; i < b->chapter_count; ++i) {
            b->chapters[i].page = read_u32(b->file);
        }
        if(saved_chapter_count > b->chapter_count) {
            uint16_t over = saved_chapter_count - b->chapter_count;
            if(!storage_file_seek(b->file, (uint32_t)over * 4u, false)) return false;
        }
    }
    b->version = 2;
    return true;
}

static bool parse_header(FBook* b) {
    char magic[FBOOK_MAGIC_LEN];
    if(storage_file_read(b->file, magic, FBOOK_MAGIC_LEN) != FBOOK_MAGIC_LEN) return false;

    if(memcmp(magic, FBOOK2_MAGIC, FBOOK_MAGIC_LEN) == 0) {
        uint16_t version = read_u16(b->file);
        if(version != 2) return false;
        return parse_header_v2(b);
    }
    if(memcmp(magic, FBOOK_MAGIC, FBOOK_MAGIC_LEN) == 0) {
        uint16_t version = read_u16(b->file);
        if(version != 1) return false;
        return parse_header_v1(b);
    }
    return false;
}

bool fbook_open(FBook* b, const char* path) {
    if(!b || !path || !path[0]) return false;
    fbook_close(b);
    b->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(b->storage, BOOKS_APP_FOLDER);
    storage_simply_mkdir(b->storage, BOOKS_LIBRARY);
    storage_simply_mkdir(b->storage, BOOKS_CACHE);
    storage_simply_mkdir(b->storage, BOOKS_PROGRESS);
    storage_simply_mkdir(b->storage, BOOKS_BOOKMARKS);

    char resolved[256];
    strncpy(resolved, path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';

    const char* dot = strrchr(path, '.');
    if(dot) {
        if(strcasecmp(dot, BOOKS_EXT_EPUB) == 0) {
            if(!fbook_import_epub(path, resolved, sizeof(resolved))) {
                fbook_close(b);
                return false;
            }
        } else if(strcasecmp(dot, BOOKS_EXT_TXT) == 0) {
            if(!fbook_import_txt(path, resolved, sizeof(resolved))) {
                fbook_close(b);
                return false;
            }
        }
    }

    strncpy(b->path, resolved, sizeof(b->path) - 1);
    b->file = storage_file_alloc(b->storage);
    if(!storage_file_open(b->file, resolved, FSAM_READ, FSOM_OPEN_EXISTING)) {
        fbook_close(b);
        return false;
    }
    if(!parse_header(b)) {
        fbook_close(b);
        return false;
    }
    if(b->text_length == 0) {
        fbook_close(b);
        return false;
    }
    return true;
}

uint32_t fbook_read(FBook* b, uint32_t offset, char* out, uint32_t max_bytes) {
    if(!b->file || offset >= b->text_length) return 0;
    if(offset + max_bytes > b->text_length) {
        max_bytes = b->text_length - offset;
    }
    if(!storage_file_seek(b->file, b->text_offset + offset, true)) return 0;
    return storage_file_read(b->file, out, max_bytes);
}

uint8_t* fbook_load_image(FBook* b, uint16_t index, uint16_t* w, uint16_t* h, uint8_t* format) {
    if(index >= b->image_count) return NULL;
    FBookImage* img = &b->images[index];
    if(img->data_len == 0 || img->data_len > 4096) return NULL;
    uint8_t* buf = malloc(img->data_len);
    if(!buf) return NULL;
    if(!storage_file_seek(b->file, img->data_offset, true)) {
        free(buf);
        return NULL;
    }
    if(storage_file_read(b->file, buf, img->data_len) != img->data_len) {
        free(buf);
        return NULL;
    }
    *w = img->w;
    *h = img->h;
    if(format) *format = img->format;
    return buf;
}

uint8_t* fbook_load_cover(FBook* b, uint16_t* w, uint16_t* h, uint8_t* format) {
    if(b->version < 2 || b->cover_data_len == 0 || b->cover_offset == 0) return NULL;
    if(b->cover_data_len > 8192) return NULL; // sanity
    uint8_t* buf = malloc(b->cover_data_len);
    if(!buf) return NULL;
    if(!storage_file_seek(b->file, b->cover_offset, true)) {
        free(buf);
        return NULL;
    }
    if(storage_file_read(b->file, buf, b->cover_data_len) != b->cover_data_len) {
        free(buf);
        return NULL;
    }
    *w = b->cover_w;
    *h = b->cover_h;
    if(format) *format = b->cover_format;
    return buf;
}

uint16_t fbook_find_chapter(const FBook* b, uint32_t offset) {
    uint16_t idx = 0;
    for(uint16_t i = 0; i < b->chapter_count; ++i) {
        if(b->chapters[i].offset <= offset) idx = i;
        else break;
    }
    return idx;
}

uint16_t fbook_next_image(const FBook* b, uint32_t offset) {
    for(uint16_t i = 0; i < b->image_count; ++i) {
        if(b->images[i].offset_in_text >= offset) return i;
    }
    return UINT16_MAX;
}

uint32_t fbook_search(FBook* b, uint32_t start_offset, const char* needle) {
    if(!needle || !*needle) return UINT32_MAX;
    size_t nlen = strlen(needle);
    if(nlen > 31) nlen = 31;
    char window[128];
    uint32_t pos = start_offset;
    while(pos < b->text_length) {
        uint32_t got = fbook_read(b, pos, window, sizeof(window) - 1);
        if(!got) break;
        window[got] = '\0';
        for(uint32_t i = 0; i + nlen <= got; ++i) {
            bool ok = true;
            for(size_t j = 0; j < nlen; ++j) {
                char a = window[i + j];
                char c = needle[j];
                if(a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
                if(c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                if(a != c) {
                    ok = false;
                    break;
                }
            }
            if(ok) return pos + i;
        }
        if(got <= nlen) break;
        pos += got - nlen; // overlap window
    }
    return UINT32_MAX;
}

static void write_u16(File* f, uint16_t v) {
    uint8_t buf[2] = {v & 0xFF, (v >> 8) & 0xFF};
    storage_file_write(f, buf, 2);
}

static void write_u32(File* f, uint32_t v) {
    uint8_t buf[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF};
    storage_file_write(f, buf, 4);
}

static void write_fixed(File* f, const char* s, size_t n) {
    uint8_t buf[128];
    if(n > sizeof(buf)) n = sizeof(buf);
    size_t l = strlen(s);
    if(l > n) l = n;
    memset(buf, 0, n);
    memcpy(buf, s, l);
    storage_file_write(f, buf, n);
}

static bool write_fbook_v1_header(
    File* out,
    const char* title,
    const char* author,
    uint32_t text_length,
    uint32_t text_offset) {
    if(storage_file_write(out, FBOOK_MAGIC, FBOOK_MAGIC_LEN) != FBOOK_MAGIC_LEN) return false;
    write_u16(out, 1);
    write_fixed(out, title, FBOOK_MAX_TITLE);
    write_fixed(out, author, FBOOK_MAX_AUTHOR);
    write_u32(out, text_offset);
    write_u32(out, text_length);
    write_u16(out, 0); // chapters
    write_u16(out, 0); // images
    return true;
}

static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void cache_path_for(const char* src, char* out, size_t out_len) {
    char base[128];
    strncpy(base, basename_of(src), sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    char* dot = strrchr(base, '.');
    if(dot) *dot = '\0';
    snprintf(out, out_len, "%s/%s.fbook", BOOKS_CACHE, base);
}

bool fbook_import_txt(const char* txt_path, char* out_path, size_t out_len) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BOOKS_CACHE);
    char dst[256];
    cache_path_for(txt_path, dst, sizeof(dst));

    File* in = storage_file_alloc(storage);
    File* out = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(in, txt_path, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_open(out, dst, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        uint32_t total = storage_file_size(in);
        storage_file_seek(in, 0, true);
        bool header_ok = write_fbook_v1_header(
            out, basename_of(txt_path), "Unknown", total, FBOOK_V1_HEADER_SIZE);

        uint8_t buf[256];
        uint32_t remaining = total;
        ok = header_ok;
        while(ok && remaining > 0) {
            uint32_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            uint16_t got = storage_file_read(in, buf, (uint16_t)want);
            if(!got) {
                ok = false;
                break;
            }
            if(storage_file_write(out, buf, got) != got) {
                ok = false;
                break;
            }
            remaining -= got;
        }
    }
    storage_file_close(in);
    storage_file_close(out);
    storage_file_free(in);
    storage_file_free(out);
    furi_record_close(RECORD_STORAGE);

    if(ok) {
        strncpy(out_path, dst, out_len - 1);
        out_path[out_len - 1] = '\0';
    }
    return ok;
}

/* EPUB on-device conversion remains a future enhancement: we look for a
 * pre-converted .fbook sidecar and otherwise prompt the user. */
bool fbook_import_epub(const char* epub_path, char* out_path, size_t out_len) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BOOKS_CACHE);
    char dst[256];
    cache_path_for(epub_path, dst, sizeof(dst));
    bool ok = storage_common_exists(storage, dst) == FSE_OK;
    furi_record_close(RECORD_STORAGE);
    if(ok) {
        strncpy(out_path, dst, out_len - 1);
        out_path[out_len - 1] = '\0';
    }
    return ok;
}

bool fbook_delete(const char* book_path) {
    if(!book_path || !*book_path) return false;
    Storage* storage = furi_record_open(RECORD_STORAGE);

    bool primary_ok = storage_simply_remove(storage, book_path);

    char cached[256];
    cache_path_for(book_path, cached, sizeof(cached));
    if(strcmp(cached, book_path) != 0) {
        storage_simply_remove(storage, cached);
    }

    const char* slash = strrchr(book_path, '/');
    const char* name = slash ? slash + 1 : book_path;
    char prg[256];
    snprintf(prg, sizeof(prg), "%s/%s.prg", BOOKS_PROGRESS, name);
    storage_simply_remove(storage, prg);

    furi_record_close(RECORD_STORAGE);
    return primary_ok;
}

uint16_t fbook_scan_library(char paths[][256], uint16_t max_paths) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BOOKS_LIBRARY);
    storage_simply_mkdir(storage, BOOKS_CACHE);

    uint16_t count = 0;
    const char* dirs[] = {BOOKS_LIBRARY, BOOKS_CACHE};
    for(size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && count < max_paths; ++d) {
        File* dir = storage_file_alloc(storage);
        if(storage_dir_open(dir, dirs[d])) {
            FileInfo info;
            char name[128];
            while(count < max_paths &&
                  storage_dir_read(dir, &info, name, sizeof(name))) {
                if(file_info_is_dir(&info)) continue;
                const char* dot = strrchr(name, '.');
                if(!dot) continue;
                if(strcasecmp(dot, BOOKS_EXT_EPUB) != 0 &&
                   strcasecmp(dot, BOOKS_EXT_FBOOK) != 0 &&
                   strcasecmp(dot, BOOKS_EXT_TXT) != 0) continue;
                snprintf(paths[count], 256, "%s/%s", dirs[d], name);
                count++;
            }
            storage_dir_close(dir);
        }
        storage_file_free(dir);
    }
    furi_record_close(RECORD_STORAGE);
    return count;
}

bool fbook_peek(const char* path,
                char* title, size_t title_len,
                char* author, size_t author_len,
                uint32_t* text_length,
                uint32_t* word_count) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char magic[FBOOK_MAGIC_LEN];
        if(storage_file_read(f, magic, FBOOK_MAGIC_LEN) == FBOOK_MAGIC_LEN) {
            uint16_t ver = read_u16(f);
            if(memcmp(magic, FBOOK2_MAGIC, FBOOK_MAGIC_LEN) == 0 && ver == 2) {
                /* v2: skip flags, then read title, author, language, text_offset, text_length, word_count */
                read_u16(f); // flags
                char tt[FBOOK_MAX_TITLE];
                char aa[FBOOK_MAX_AUTHOR];
                if(storage_file_read(f, tt, FBOOK_MAX_TITLE) == FBOOK_MAX_TITLE &&
                   storage_file_read(f, aa, FBOOK_MAX_AUTHOR) == FBOOK_MAX_AUTHOR) {
                    /* Skip language. */
                    storage_file_seek(f, FBOOK_MAX_LANG, false);
                    uint32_t to = read_u32(f);
                    uint32_t tl = read_u32(f);
                    uint32_t wc = read_u32(f);
                    (void)to;
                    if(title) {
                        strncpy(title, tt, title_len - 1);
                        title[title_len - 1] = '\0';
                    }
                    if(author) {
                        strncpy(author, aa, author_len - 1);
                        author[author_len - 1] = '\0';
                    }
                    if(text_length) *text_length = tl;
                    if(word_count) *word_count = wc;
                    ok = true;
                }
            } else if(memcmp(magic, FBOOK_MAGIC, FBOOK_MAGIC_LEN) == 0 && ver == 1) {
                char tt[FBOOK_MAX_TITLE];
                char aa[FBOOK_MAX_AUTHOR];
                if(storage_file_read(f, tt, FBOOK_MAX_TITLE) == FBOOK_MAX_TITLE &&
                   storage_file_read(f, aa, FBOOK_MAX_AUTHOR) == FBOOK_MAX_AUTHOR) {
                    uint32_t to = read_u32(f);
                    uint32_t tl = read_u32(f);
                    (void)to;
                    if(title) {
                        strncpy(title, tt, title_len - 1);
                        title[title_len - 1] = '\0';
                    }
                    if(author) {
                        strncpy(author, aa, author_len - 1);
                        author[author_len - 1] = '\0';
                    }
                    if(text_length) *text_length = tl;
                    if(word_count) *word_count = tl / 6; // crude estimate
                    ok = true;
                }
            }
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

uint8_t* fbook_peek_cover(const char* path, uint16_t* w, uint16_t* h, uint8_t* format) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    uint8_t* buf = NULL;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char magic[FBOOK_MAGIC_LEN];
        if(storage_file_read(f, magic, FBOOK_MAGIC_LEN) == FBOOK_MAGIC_LEN) {
            uint16_t ver = read_u16(f);
            if(memcmp(magic, FBOOK2_MAGIC, FBOOK_MAGIC_LEN) == 0 && ver == 2) {
                read_u16(f); // flags
                /* Skip title + author + language */
                storage_file_seek(f, FBOOK_MAX_TITLE + FBOOK_MAX_AUTHOR + FBOOK_MAX_LANG, false);
                read_u32(f); // text_offset
                read_u32(f); // text_length
                read_u32(f); // word_count
                read_u16(f); // chapter_count
                read_u16(f); // image_count
                uint32_t cover_offset = read_u32(f);
                uint16_t cw = read_u16(f);
                uint16_t ch = read_u16(f);
                uint32_t clen = read_u32(f);
                uint8_t cfmt = read_u8(f);
                if(cover_offset && clen && clen <= 8192 && cw && ch) {
                    if(storage_file_seek(f, cover_offset, true)) {
                        buf = malloc(clen);
                        if(buf) {
                            if(storage_file_read(f, buf, clen) != clen) {
                                free(buf);
                                buf = NULL;
                            } else {
                                if(w) *w = cw;
                                if(h) *h = ch;
                                if(format) *format = cfmt;
                            }
                        }
                    }
                }
            }
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return buf;
}
