#include "reader_view.h"

#include <furi.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <string.h>
#include <stdlib.h>

#define READER_W 128
#define READER_H 64

#define PAGE_STACK 64

typedef struct {
    uint32_t offsets[PAGE_STACK];
    uint16_t count;
} PageStack;

struct ReaderView {
    View* view;
    ReaderEventCallback event_cb;
    void* event_ctx;
    FuriTimer* anim_timer;
    FuriTimer* auto_timer;
    /* Optional - reader scene hands this in so input_callback can re-suppress
     * the backlight after every keypress when PowerSaver is on. */
    NotificationApp* notifications;
};

typedef struct {
    FBook* book;
    const BookSettings* settings;
    BookProgress progress; // copied in

    uint32_t page_offset;      // start of current page
    uint32_t page_end_offset;  // start of next page
    uint32_t page_number;
    uint32_t total_pages_estimate; // updated after compute_page_end

    PageStack stack;

    // animation
    int8_t anim_dir; // -1 back, +1 forward, 0 none
    uint8_t anim_progress; // 0..100
    /* Snapshot of the page we're animating away from so both pages can be
     * rendered side-by-side during the transition. Without this the old
     * page would simply disappear and the slide would be invisible. */
    char prev_cache[512];
    uint16_t prev_cache_len;
    uint32_t prev_page_offset;
    uint32_t prev_page_end_offset;

    // small draw cache of visible lines
    char cache[512];
    uint16_t cache_len;

    // status
    bool show_bookmark_flash;
    uint32_t bookmark_flash_until;
    bool menu_prompt;

    // sleep timer
    uint32_t sleep_deadline_tick; // 0 = disabled
    bool sleep_expired;
} ReaderModel;

static Font font_for_settings(const BookSettings* s) {
    switch(s->font_family) {
    case FontFamilySerif: return FontPrimary;
    case FontFamilySans:  return FontSecondary;
    case FontFamilyDefault:
    default:
        switch(s->text_size) {
        case TextSizeTiny:
        case TextSizeSmall:
            return FontSecondary;
        case TextSizeMedium:
        case TextSizeLarge:
            return FontPrimary;
        }
        return FontSecondary;
    }
}

static uint8_t line_height_for_size(TextSize t) {
    switch(t) {
    case TextSizeTiny:   return 7;
    case TextSizeSmall:  return 9;
    case TextSizeMedium: return 11;
    case TextSizeLarge:  return 14;
    }
    return 9;
}

static uint8_t max_chars_per_line(TextSize t) {
    switch(t) {
    case TextSizeTiny:   return 32;
    case TextSizeSmall:  return 26;
    case TextSizeMedium: return 19;
    case TextSizeLarge:  return 14;
    }
    return 26;
}

/* Per-book hard ceiling on inline image height. v1 keeps the original 32-px
 * cap; v2 used to ask for the full screen but that turned out to provoke a
 * fault in the field on the very first open of certain books, so we now keep
 * v2 at 48 px - still better than v1's 32, and well within the headroom the
 * canvas/word-wrap maths was tested with. */
static uint8_t inline_image_max_h(const FBook* b) {
    return (b && b->version >= 2) ? 48u : 32u;
}

/* Plate-page mode (whole screen given to one image) was disabled after
 * crash reports right after opening v2 books with cover-page art. The
 * normal reserve+text path renders covers the same way but without the
 * special-case branching. */
static bool is_plate_page(const ReaderModel* m) {
    (void)m;
    return false;
}

/* Height in pixels reserved at the top of the page for an inline image on the
 * current page, matching what draw_image_on_page will actually render. */
static uint8_t image_top_reserve(const ReaderModel* m) {
    if(!m->settings->show_images) return 0;
    if(m->settings->power_mode == PowerModePowerSaver) return 0;
    if(!m->book || m->book->image_count == 0) return 0;
    uint16_t idx = fbook_next_image(m->book, m->page_offset);
    if(idx == UINT16_MAX) return 0;
    uint32_t off = m->book->images[idx].offset_in_text;
    if(off < m->page_offset) return 0;
    if(off >= m->page_end_offset) return 0;
    uint16_t h = m->book->images[idx].h;
    uint8_t cap = inline_image_max_h(m->book);
    if(h > cap) h = cap;
    return (uint8_t)h;
}

static uint32_t compute_page_end_for(
    FBook* book,
    const BookSettings* settings,
    uint32_t page_offset,
    char* cache,
    uint16_t cache_size,
    uint16_t* out_cache_len) {
    if(!book || !settings || !cache || cache_size == 0) return page_offset;

    uint16_t cache_len = (uint16_t)fbook_read(book, page_offset, cache, cache_size - 1);
    cache[cache_len] = '\0';
    if(out_cache_len) *out_cache_len = cache_len;

    uint8_t lh = line_height_for_size(settings->text_size);
    uint8_t avail_h = READER_H;
    if(settings->show_progress_bar && avail_h > 3) avail_h -= 3;

    /* Reserve room for an inline image on this page, using cache_len as the
     * upper bound for what this page can cover. We also subtract 2px of gap
     * between the image and the first line of text so descenders don't touch. */
    uint8_t reserve = 0;
    if(settings->show_images && settings->power_mode != PowerModePowerSaver) {
        uint16_t img_idx = fbook_next_image(book, page_offset);
        if(img_idx != UINT16_MAX &&
           book->images[img_idx].offset_in_text < page_offset + cache_len) {
            uint16_t h = book->images[img_idx].h;
            uint8_t cap = inline_image_max_h(book);
            if(h > cap) h = cap;
            reserve = (uint8_t)h;
            if(reserve + 2 < avail_h) reserve += 2;
        }
    }
    /* Page-number overlay sits in the top-right; reserve the first 8px so
     * it doesn't overlap the first line of text. */
    if(settings->show_page_number && reserve < 8) reserve = 8;
    uint8_t text_h = avail_h > reserve ? avail_h - reserve : lh;
    uint8_t max_lines = (uint8_t)(text_h / lh);
    if(max_lines < 1) max_lines = 1;

    uint8_t mcpl = max_chars_per_line(settings->text_size);

    uint16_t i = 0;
    uint16_t lines = 0;
    uint16_t last_break = 0;

    while(i < cache_len && lines < max_lines) {
        // newline handling (hard break)
        uint16_t line_start = i;
        uint16_t col = 0;
        while(i < cache_len && cache[i] != '\n' && col < mcpl) {
            i++;
            col++;
        }
        if(i < cache_len && cache[i] == '\n') {
            i++;
        } else if(col == mcpl && i < cache_len) {
            // back up to previous space for word wrap
            uint16_t j = i;
            while(j > line_start && cache[j] != ' ') j--;
            if(j > line_start) i = j + 1;
        }
        last_break = i;
        lines++;
    }

    if(last_break == 0) last_break = cache_len; // fallback, prevents infinite loop
    uint32_t page_end_offset = page_offset + last_break;
    if(page_end_offset > book->text_length) page_end_offset = book->text_length;
    return page_end_offset;
}

/* Compute page end offset by word-wrapping in the cache. */
static void compute_page_end(ReaderModel* m) {
    /* Plate page: a v2 image that fills the screen takes the page on its
     * own; advance past the single byte the converter inserts at the image
     * marker so the next page resumes with text. */
    if(is_plate_page(m)) {
        m->cache_len = fbook_read(m->book, m->page_offset, m->cache, sizeof(m->cache) - 1);
        m->cache[m->cache_len] = '\0';
        m->page_end_offset = m->page_offset + 1;
        if(m->page_end_offset > m->book->text_length) {
            m->page_end_offset = m->book->text_length;
        }
        return;
    }

    m->page_end_offset = compute_page_end_for(
        m->book,
        m->settings,
        m->page_offset,
        m->cache,
        sizeof(m->cache),
        &m->cache_len);
}

/* Capture the current page's text into the snapshot used by the transition
 * animation, so the outgoing page is still drawable while the incoming page
 * slides in. Called immediately before page_offset is updated. */
static void snapshot_current_page(ReaderModel* m) {
    uint16_t n = m->cache_len;
    if(n > sizeof(m->prev_cache)) n = sizeof(m->prev_cache);
    memcpy(m->prev_cache, m->cache, n);
    m->prev_cache_len = n;
    m->prev_page_offset = m->page_offset;
    m->prev_page_end_offset = m->page_end_offset;
}

static void push_page(PageStack* s, uint32_t offset) {
    if(s->count < PAGE_STACK) {
        s->offsets[s->count++] = offset;
    } else {
        for(int i = 0; i < PAGE_STACK - 1; ++i) s->offsets[i] = s->offsets[i + 1];
        s->offsets[PAGE_STACK - 1] = offset;
    }
}

static bool pop_page(PageStack* s, uint32_t* out) {
    if(s->count == 0) return false;
    *out = s->offsets[--s->count];
    return true;
}

static void draw_night(Canvas* c, const BookSettings* s) {
    if(s && s->night_mode) {
        /* Fill the background with pixels-on so subsequent ColorWhite
         * (pixels-off) draws show through as light text on a dark page. */
        canvas_set_color(c, ColorBlack);
        canvas_draw_box(c, 0, 0, READER_W, READER_H);
        canvas_set_color(c, ColorWhite);
    }
}

/* Draw lines from a buffer at a given screen x-offset. Used for both the
 * current page and (during a slide animation) the previous page snapshot.
 * `reserve_top` is the pixel height to skip at the top of the screen for an
 * inline image; `text_len` is the number of bytes in `buf` that belong to the
 * page (i.e. up to its computed page_end). */
static void draw_text_buffer(
    Canvas* c,
    const ReaderModel* m,
    const char* buf,
    uint16_t text_len,
    int16_t x_offset,
    uint8_t reserve_top) {
    canvas_set_font(c, font_for_settings(m->settings));
    uint8_t lh = line_height_for_size(m->settings->text_size);
    /* Apply line spacing setting. */
    switch(m->settings->line_spacing) {
    case LineSpacingTight:  if(lh > 1) lh -= 1; break;
    case LineSpacingNormal: break;
    case LineSpacingLoose:  lh += 1; break;
    case LineSpacingDouble: lh += 3; break;
    }
    uint8_t mcpl = max_chars_per_line(m->settings->text_size);
    /* Margin shifts the inner column. Compact (no margin) lets us fit a few
     * extra characters per line at the smaller text sizes. */
    uint8_t left_pad = 1;
    switch(m->settings->margin) {
    case MarginCompact: left_pad = 0; break;
    case MarginNormal:  left_pad = 1; break;
    case MarginWide:    left_pad = 4; if(mcpl > 4) mcpl -= 2; break;
    }

    uint16_t i = 0;
    uint16_t lines = 0;
    int16_t y = reserve_top + lh - 1;
    uint8_t avail_h = READER_H > reserve_top ? READER_H - reserve_top : lh;
    if(m->settings->show_progress_bar && avail_h > 3) avail_h -= 3;
    uint8_t max_lines = (uint8_t)(avail_h / lh);
    if(max_lines < 1) max_lines = 1;

    char line[64];
    uint16_t line_end = text_len;
    if(line_end > sizeof(m->cache) - 1) line_end = sizeof(m->cache) - 1;

    while(i < line_end && lines < max_lines) {
        uint16_t line_start = i;
        uint16_t col = 0;
        while(i < line_end && buf[i] != '\n' && col < mcpl) {
            i++;
            col++;
        }
        uint16_t end = i;
        bool hard_nl = false;
        if(i < line_end && buf[i] == '\n') {
            hard_nl = true;
            i++;
        } else if(col == mcpl && i < line_end) {
            uint16_t j = i;
            while(j > line_start && buf[j] != ' ') j--;
            if(j > line_start) {
                end = j;
                i = j + 1;
            }
        }
        uint16_t n = end - line_start;
        if(n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, &buf[line_start], n);
        line[n] = '\0';
        while(n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) {
            line[--n] = '\0';
        }
        if(m->settings->night_mode) canvas_set_color(c, ColorWhite);
        else canvas_set_color(c, ColorBlack);
        canvas_draw_str(c, left_pad + x_offset, y, line);
        y += lh;
        lines++;
        if(!hard_nl && i < line_end && buf[i] == ' ') i++;
    }
}

static void draw_text_page(Canvas* c, ReaderModel* m, int16_t x_offset) {
    uint8_t reserve = image_top_reserve(m);
    if(reserve > 0 && reserve + 2 < READER_H) reserve += 2;
    /* Push text below the page-number overlay when it's on. Mirrors the
     * reserve logic in compute_page_end so line counts stay consistent. */
    if(m->settings->show_page_number && reserve < 8) reserve = 8;
    uint16_t text_len = (uint16_t)(m->page_end_offset - m->page_offset);
    draw_text_buffer(c, m, m->cache, text_len, x_offset, reserve);
}

static void draw_progress_bar(Canvas* c, const FBook* b, uint32_t offset, bool night) {
    if(b->text_length == 0) return;
    uint32_t pct = (offset * 100) / b->text_length;
    if(pct > 100) pct = 100;
    int16_t w = (int16_t)((pct * (READER_W - 2)) / 100);
    if(night) canvas_set_color(c, ColorWhite);
    else canvas_set_color(c, ColorBlack);
    canvas_draw_frame(c, 0, READER_H - 2, READER_W, 2);
    canvas_draw_box(c, 1, READER_H - 2, w, 2);
}

/* Draws "p N  P%" in the top-right; used when show_page_number is enabled.
 * Kept tiny so it doesn't eat into the visible text area. */
static void draw_page_number(Canvas* c, const ReaderModel* m) {
    if(!m->book || m->book->text_length == 0) return;
    char buf[24];
    uint32_t pct = (m->page_offset * 100) / m->book->text_length;
    if(pct > 100) pct = 100;
    snprintf(buf, sizeof(buf), "p%lu  %lu%%",
             (unsigned long)(m->page_number + 1),
             (unsigned long)pct);
    canvas_set_font(c, FontSecondary);
    if(m->settings->night_mode) canvas_set_color(c, ColorWhite);
    else canvas_set_color(c, ColorBlack);
    canvas_draw_str_aligned(c, READER_W - 1, 1, AlignRight, AlignTop, buf);
}

static void draw_image_on_page(Canvas* c, ReaderModel* m) {
    if(!m->settings->show_images) return;
    if(m->settings->power_mode == PowerModePowerSaver) return;
    uint16_t idx = fbook_next_image(m->book, m->page_offset);
    if(idx == UINT16_MAX) return;
    if(m->book->images[idx].offset_in_text >= m->page_end_offset) return;
    if(m->book->images[idx].offset_in_text < m->page_offset) return;

    /* Guard against corrupt dimensions before we ask the loader to malloc and
     * read (w*h)/8 bytes for us. A full-page image at this resolution is at
     * most READER_W * 32 / 8 = 512 bytes. */
    uint16_t stored_w = m->book->images[idx].w;
    uint16_t stored_h = m->book->images[idx].h;
    if(stored_w == 0 || stored_h == 0) return;
    if(stored_w > READER_W || stored_h > READER_H) return;

    uint16_t w, h;
    uint8_t fmt = 0;
    uint8_t* data = fbook_load_image(m->book, idx, &w, &h, &fmt);
    if(!data) return;
    (void)fmt; /* 2bpp rendering is added below; for now treat all as XBM. */

    uint16_t draw_w = w > READER_W ? READER_W : w;
    uint8_t h_cap = inline_image_max_h(m->book);
    uint16_t draw_h = h > h_cap ? h_cap : h;
    int16_t x = (READER_W - draw_w) / 2;
    /* Plate pages: vertically centre the image so a portrait cover doesn't
     * sit awkwardly at the top of the screen. */
    int16_t y = is_plate_page(m) ? (int16_t)((READER_H - draw_h) / 2) : 0;
    canvas_draw_xbm(c, x, y, draw_w, draw_h, data);
    free(data);
}

static void render_callback(Canvas* c, void* ctx) {
    ReaderModel* m = ctx;
    canvas_clear(c);

    /* Defensive: settings can be NULL during the brief window between view
     * allocation and the reader scene wiring it up. The original code only
     * checked m->book and would dereference m->settings unconditionally
     * later; on rare scheduling paths (notably right after the user changes
     * power_mode in Settings, which fires a notification that can re-pump
     * the GUI) this produced a bus error. */
    if(!m->settings) {
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, READER_W / 2, READER_H / 2, AlignCenter, AlignCenter, "...");
        return;
    }

    draw_night(c, m->settings);

    if(!m->book || !m->book->file) {
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, READER_W / 2, READER_H / 2, AlignCenter, AlignCenter, "No book");
        return;
    }

    bool animate = m->anim_dir != 0 &&
                   m->settings->page_animation != PageAnimNone &&
                   m->settings->power_mode != PowerModePowerSaver;

    /* Compute prev_text_len once and clamp it to the cache size before either
     * animation branch uses it - the subtraction is unsigned and a stale
     * snapshot can underflow into a giant value that walks off prev_cache. */
    uint16_t prev_text_len = 0;
    if(m->prev_page_end_offset >= m->prev_page_offset) {
        prev_text_len =
            (uint16_t)(m->prev_page_end_offset - m->prev_page_offset);
    }
    if(prev_text_len > sizeof(m->prev_cache)) prev_text_len = sizeof(m->prev_cache);

    if(animate && m->settings->page_animation == PageAnimSlide) {
        /* Slide: outgoing page slides off, incoming slides in. anim_progress
         * runs 0..100. dir +1 means forward (next page in from right);
         * dir -1 means back (previous page in from left). */
        int16_t s = (int16_t)((int32_t)m->anim_progress * READER_W / 100);
        if(m->anim_dir > 0) {
            draw_text_buffer(c, m, m->prev_cache, prev_text_len, -s, 0);
            draw_text_page(c, m, READER_W - s);
        } else {
            draw_text_buffer(c, m, m->prev_cache, prev_text_len, s, 0);
            draw_text_page(c, m, s - READER_W);
        }
    } else if(animate && m->settings->page_animation == PageAnimFade) {
        /* Fade on a 1-bit screen: a vertical wipe gives the user a real sense
         * of progress without trying to fake alpha. Top half of the page is
         * the new content, bottom half is the old, with the boundary moving
         * downward as anim_progress runs 0..100. */
        int16_t boundary = (int16_t)((int32_t)m->anim_progress * READER_H / 100);
        canvas_set_bitmap_mode(c, false);
        /* Draw new page first (it will cover everything we plan to keep). */
        draw_text_page(c, m, 0);
        /* Then re-draw the bottom strip with the old page. */
        if(boundary < READER_H) {
            /* Clip by carving out the top "boundary" rows: for the bottom we
             * draw the prev page, and the top half from the new page is
             * already on the canvas above. */
            canvas_set_color(c, m->settings->night_mode ? ColorBlack : ColorWhite);
            canvas_draw_box(c, 0, boundary, READER_W, READER_H - boundary);
            canvas_set_color(c, m->settings->night_mode ? ColorWhite : ColorBlack);
            /* Draw prev_cache shifted down by `boundary`-many lines: simplest
             * faithful approximation is to render it from y=0 and rely on the
             * fact that the box above covered the new page below the
             * boundary, so the prev page fills the bottom strip. */
            draw_text_buffer(c, m, m->prev_cache, prev_text_len, 0, 0);
        }
    } else if(animate && m->settings->page_animation == PageAnimCurl) {
        /* Curl: diagonal wipe from the bottom-right (forward) or top-left
         * (back) corner, simulating a page lifting off. anim_progress sweeps
         * a diagonal line across the screen; one side draws the new page,
         * the other keeps the old. */
        int16_t diag = (int16_t)((int32_t)m->anim_progress *
                                 (READER_W + READER_H) / 100);
        /* Old page underneath. */
        draw_text_buffer(c, m, m->prev_cache, prev_text_len, 0, 0);
        /* New page revealed where x + y < diag (forward) or > diag (back). */
        for(int16_t y = 0; y < READER_H; y++) {
            int16_t cut = (m->anim_dir > 0) ? (diag - y) : (READER_W - (diag - y));
            if(cut <= 0) continue;
            if(cut > READER_W) cut = READER_W;
            /* For forward: clear left strip then redraw new on top.
             * For back: clear right strip. */
            canvas_set_color(c, m->settings->night_mode ? ColorBlack : ColorWhite);
            if(m->anim_dir > 0) {
                canvas_draw_line(c, 0, y, cut - 1, y);
            } else {
                canvas_draw_line(c, READER_W - cut, y, READER_W - 1, y);
            }
        }
        canvas_set_color(c, m->settings->night_mode ? ColorWhite : ColorBlack);
        /* Diagonal hairline between old and new for visual punctuation. */
        if(m->anim_dir > 0) {
            canvas_draw_line(c, diag, 0, 0, diag);
        } else {
            canvas_draw_line(c, READER_W - diag, 0, READER_W, diag);
        }
        /* Re-draw the new page in the revealed half: simplest is to draw
         * full and let the box above hide the unrevealed strip - but we've
         * already cleared the unrevealed half, so just draw on top. */
        draw_text_page(c, m, 0);
    } else {
        draw_text_page(c, m, 0);
    }

    draw_image_on_page(c, m);

    if(m->settings->show_progress_bar) {
        draw_progress_bar(c, m->book, m->page_offset, m->settings->night_mode);
    }
    if(m->settings->show_page_number) {
        draw_page_number(c, m);
    }

    if(m->show_bookmark_flash && furi_get_tick() < m->bookmark_flash_until) {
        canvas_set_color(c, m->settings->night_mode ? ColorWhite : ColorBlack);
        canvas_draw_box(c, READER_W - 8, 0, 6, 8);
    } else {
        m->show_bookmark_flash = false;
    }
}

static void anim_timer_cb(void* ctx) {
    ReaderView* r = ctx;
    bool fire_sleep = false;
    with_view_model(
        r->view, ReaderModel * m, {
            if(m->anim_dir != 0) {
                if(m->anim_progress >= 100) {
                    m->anim_dir = 0;
                    m->anim_progress = 0;
                } else {
                    m->anim_progress = (uint8_t)(m->anim_progress + 20);
                    if(m->anim_progress > 100) m->anim_progress = 100;
                }
            }
            if(m->sleep_deadline_tick && !m->sleep_expired &&
               furi_get_tick() >= m->sleep_deadline_tick) {
                m->sleep_expired = true;
                fire_sleep = true;
            }
        },
        true);
    if(fire_sleep && r->event_cb) {
        r->event_cb(ReaderEventBack, r->event_ctx);
    }
}

static void auto_timer_cb(void* ctx) {
    ReaderView* r = ctx;
    with_view_model(
        r->view, ReaderModel * m, {
            if(m->settings && m->settings->auto_scroll && m->book &&
               m->page_end_offset < m->book->text_length) {
                snapshot_current_page(m);
                push_page(&m->stack, m->page_offset);
                m->page_offset = m->page_end_offset;
                m->page_number++;
                compute_page_end(m);
                m->anim_dir = 1;
                m->anim_progress = 0;
            }
        },
        true);
}

static void emit_event(ReaderView* r, ReaderPublicEvent ev) {
    if(r->event_cb) r->event_cb(ev, r->event_ctx);
}

/* Long-press helpers: jump to the previous/next chapter boundary. Bound to
 * long-press Left / long-press Right in input_callback. */
static void jump_to_chapter_offset(ReaderModel* m, uint32_t offset) {
    snapshot_current_page(m);
    push_page(&m->stack, m->page_offset);
    m->page_offset = offset;
    if(m->page_offset > m->book->text_length) m->page_offset = m->book->text_length;
    compute_page_end(m);
    m->anim_dir = 0;
    m->anim_progress = 0;
}

static void jump_prev_chapter(ReaderModel* m) {
    if(!m->book || m->book->chapter_count == 0) return;
    uint16_t cur = fbook_find_chapter(m->book, m->page_offset);
    /* If we're past a chapter's start, jump to the start of this one. */
    if(m->book->chapters[cur].offset < m->page_offset) {
        jump_to_chapter_offset(m, m->book->chapters[cur].offset);
        return;
    }
    if(cur == 0) return;
    jump_to_chapter_offset(m, m->book->chapters[cur - 1].offset);
}

static void jump_next_chapter(ReaderModel* m) {
    if(!m->book || m->book->chapter_count == 0) return;
    uint16_t cur = fbook_find_chapter(m->book, m->page_offset);
    if(cur + 1 >= m->book->chapter_count) return;
    jump_to_chapter_offset(m, m->book->chapters[cur + 1].offset);
}

static bool input_callback(InputEvent* evt, void* ctx) {
    ReaderView* r = ctx;
    bool consumed = false;

    if(evt->type == InputTypeLong) {
        if(evt->key == InputKeyLeft) {
            with_view_model(
                r->view, ReaderModel * m, { jump_prev_chapter(m); }, true);
            return true;
        }
        if(evt->key == InputKeyRight) {
            with_view_model(
                r->view, ReaderModel * m, { jump_next_chapter(m); }, true);
            return true;
        }
    }

    if(evt->type == InputTypeShort) {
        with_view_model(
            r->view, ReaderModel * m, {
                switch(evt->key) {
                case InputKeyRight:
                    if(m->page_end_offset < m->book->text_length) {
                        snapshot_current_page(m);
                        push_page(&m->stack, m->page_offset);
                        m->page_offset = m->page_end_offset;
                        m->page_number++;
                        compute_page_end(m);
                        m->anim_dir = 1;
                        m->anim_progress = 0;
                    }
                    consumed = true;
                    break;
                case InputKeyLeft: {
                    uint32_t prev;
                    if(pop_page(&m->stack, &prev)) {
                        snapshot_current_page(m);
                        m->page_offset = prev;
                        if(m->page_number > 0) m->page_number--;
                        compute_page_end(m);
                        m->anim_dir = -1;
                        m->anim_progress = 0;
                    }
                    consumed = true;
                    break;
                }
                case InputKeyUp:
                    // emit bookmark
                    m->show_bookmark_flash = true;
                    m->bookmark_flash_until = furi_get_tick() + 800;
                    consumed = true;
                    break;
                case InputKeyDown:
                    // search next
                    break;
                case InputKeyOk:
                    break;
                default: break;
                }
            },
            true);

        // events outside the model
        if(evt->key == InputKeyOk) {
            emit_event(r, ReaderEventMenu);
            consumed = true;
        } else if(evt->key == InputKeyBack) {
            emit_event(r, ReaderEventBack);
            consumed = true;
        } else if(evt->key == InputKeyUp) {
            emit_event(r, ReaderEventBookmark);
            consumed = true;
        } else if(evt->key == InputKeyDown) {
            emit_event(r, ReaderEventToc);
            consumed = true;
        }
    }

    /* PowerSaver re-suppression: the firmware's input pipeline turns the
     * backlight back on at every event. Without an _enforce_off message we
     * have to keep reapplying the one-shot _off after we've handled the key.
     * Done outside the model lock to avoid blocking the GUI thread on the
     * notification record. */
    if(r->notifications) {
        bool save = false;
        with_view_model(
            r->view, ReaderModel * m,
            { save = m->settings && m->settings->power_mode == PowerModePowerSaver; },
            false);
        if(save) {
            notification_message(r->notifications, &sequence_display_backlight_off);
        }
    }

    return consumed;
}

ReaderView* reader_view_alloc(void) {
    ReaderView* r = malloc(sizeof(ReaderView));
    memset(r, 0, sizeof(*r));
    r->view = view_alloc();
    view_allocate_model(r->view, ViewModelTypeLocking, sizeof(ReaderModel));
    with_view_model(
        r->view, ReaderModel * m, { memset(m, 0, sizeof(*m)); }, false);
    view_set_context(r->view, r);
    view_set_draw_callback(r->view, render_callback);
    view_set_input_callback(r->view, input_callback);

    r->anim_timer = furi_timer_alloc(anim_timer_cb, FuriTimerTypePeriodic, r);
    furi_timer_start(r->anim_timer, furi_ms_to_ticks(30));
    r->auto_timer = furi_timer_alloc(auto_timer_cb, FuriTimerTypePeriodic, r);
    return r;
}

void reader_view_free(ReaderView* r) {
    if(!r) return;
    furi_timer_stop(r->anim_timer);
    furi_timer_free(r->anim_timer);
    furi_timer_stop(r->auto_timer);
    furi_timer_free(r->auto_timer);
    view_free(r->view);
    free(r);
}

View* reader_view_get_view(ReaderView* r) { return r->view; }

void reader_view_set_book(ReaderView* r, FBook* book) {
    with_view_model(
        r->view, ReaderModel * m, {
            m->book = book;
            m->page_offset = 0;
            m->page_end_offset = 0;
            m->page_number = 0;
            m->stack.count = 0;
            if(book && m->settings) compute_page_end(m);
        },
        true);
}

void reader_view_set_settings(ReaderView* r, const BookSettings* settings) {
    with_view_model(
        r->view, ReaderModel * m, {
            m->settings = settings;
            if(settings->auto_scroll) {
                furi_timer_start(r->auto_timer, furi_ms_to_ticks(1000u * settings->auto_scroll_speed));
            } else {
                furi_timer_stop(r->auto_timer);
            }
            /* (Re)compute the sleep timer deadline whenever settings change.
             * 0 = disabled. Timer is checked from anim_timer_cb. */
            if(settings->sleep_timer_minutes == 0) {
                m->sleep_deadline_tick = 0;
            } else {
                uint32_t freq = furi_kernel_get_tick_frequency();
                m->sleep_deadline_tick =
                    furi_get_tick() +
                    (uint32_t)settings->sleep_timer_minutes * 60u * freq;
            }
            m->sleep_expired = false;
            if(m->book) compute_page_end(m);
        },
        true);
}

void reader_view_set_progress(ReaderView* r, const BookProgress* progress) {
    with_view_model(
        r->view, ReaderModel * m, {
            m->progress = *progress;
            m->page_offset = progress->offset;
            m->page_number = progress->page;
            m->stack.count = 0;
            if(m->book && m->settings) compute_page_end(m);
        },
        true);
}

uint32_t reader_view_get_offset(const ReaderView* r) {
    uint32_t out = 0;
    with_view_model(
        r->view, ReaderModel * m, { out = m->page_offset; }, false);
    return out;
}

uint32_t reader_view_get_page(const ReaderView* r) {
    uint32_t out = 0;
    with_view_model(
        r->view, ReaderModel * m, { out = m->page_number; }, false);
    return out;
}

void reader_view_jump_to(ReaderView* r, uint32_t offset) {
    with_view_model(
        r->view, ReaderModel * m, {
            push_page(&m->stack, m->page_offset);
            m->page_offset = offset;
            if(m->book && m->settings) compute_page_end(m);
        },
        true);
}

void reader_view_build_page_map(
    FBook* book,
    const BookSettings* settings,
    const uint32_t* offsets,
    uint16_t count,
    uint32_t* pages) {
    if(!pages) return;
    for(uint16_t i = 0; i < count; ++i) pages[i] = 0;
    if(!book || !settings || !offsets || count == 0) return;

    char cache[512];
    uint32_t page_offset = 0;
    uint32_t page = 0;
    uint16_t next_target = 0;

    while(next_target < count) {
        uint32_t target = offsets[next_target];
        if(target > book->text_length) target = book->text_length;

        if(target <= page_offset) {
            pages[next_target++] = page;
            continue;
        }

        uint32_t next_offset = compute_page_end_for(
            book, settings, page_offset, cache, sizeof(cache), NULL);
        if(next_offset <= page_offset) {
            next_offset = page_offset + 1;
            if(next_offset > book->text_length) next_offset = book->text_length;
        }

        if(target < next_offset) {
            pages[next_target++] = page;
            continue;
        }

        page_offset = next_offset;
        page++;

        if(page_offset >= book->text_length) {
            while(next_target < count) {
                pages[next_target++] = page;
            }
            break;
        }
    }
}

void reader_view_set_event_callback(ReaderView* r, ReaderEventCallback cb, void* ctx) {
    r->event_cb = cb;
    r->event_ctx = ctx;
}

void reader_view_set_notifications(ReaderView* r, NotificationApp* notifications) {
    r->notifications = notifications;
}
