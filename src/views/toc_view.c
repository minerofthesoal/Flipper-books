#include "toc_view.h"

#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TOC_ROW_HEIGHT 13
#define TOC_HEADER_H   11
#define TOC_VISIBLE    4
#define TOC_TEXT_W     122
#define TOC_LABEL_LEN  96

typedef struct {
    char text[TOC_LABEL_LEN];
} TocEntry;

typedef struct {
    TocEntry entries[FBOOK_MAX_CHAPTERS];
    uint16_t count;
    uint16_t selected;
    uint16_t scroll_top;
    uint16_t text_scroll_px;
    uint16_t text_scroll_max_px;
    char header[18];
    char empty_text[32];
} TocModel;

struct TocView {
    View* view;
    FuriTimer* scroll_timer;
    TocViewSelectCb cb;
    void* cb_ctx;
};

static uint16_t toc_scroll_estimate(const char* text) {
    size_t len = strlen(text);
    uint32_t w = (uint32_t)len * 6u;
    return w > TOC_TEXT_W ? (uint16_t)(w - TOC_TEXT_W + 12u) : 0;
}

static void toc_reset_text_scroll(TocModel* m) {
    m->text_scroll_px = 0;
    m->text_scroll_max_px =
        m->selected < m->count ? toc_scroll_estimate(m->entries[m->selected].text) : 0;
}

static void render_callback(Canvas* c, void* ctx) {
    TocModel* m = ctx;
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 8, m->header[0] ? m->header : "Contents");
    canvas_draw_line(c, 0, 10, 127, 10);

    if(m->count == 0) {
        canvas_draw_str_aligned(
            c,
            64,
            36,
            AlignCenter,
            AlignCenter,
            m->empty_text[0] ? m->empty_text : "(no chapters)");
        return;
    }

    if(m->selected < m->scroll_top) m->scroll_top = m->selected;
    if(m->selected >= m->scroll_top + TOC_VISIBLE) {
        m->scroll_top = (uint16_t)(m->selected - TOC_VISIBLE + 1);
    }

    uint16_t selected_width = canvas_string_width(c, m->entries[m->selected].text);
    m->text_scroll_max_px =
        selected_width > TOC_TEXT_W ? (uint16_t)(selected_width - TOC_TEXT_W + 12u) : 0;
    if(m->text_scroll_px > m->text_scroll_max_px) m->text_scroll_px = 0;

    for(uint16_t row = 0; row < TOC_VISIBLE; ++row) {
        uint16_t idx = m->scroll_top + row;
        if(idx >= m->count) break;

        int16_t y = TOC_HEADER_H + (int16_t)row * TOC_ROW_HEIGHT;
        bool selected = idx == m->selected;
        if(selected) {
            canvas_draw_box(c, 0, y, 126, TOC_ROW_HEIGHT);
            canvas_set_color(c, ColorWhite);
        } else {
            canvas_set_color(c, ColorBlack);
        }

        int16_t x = 2;
        if(selected && m->text_scroll_px > 0) {
            x -= (int16_t)m->text_scroll_px;
        }
        canvas_draw_str(c, x, y + 9, m->entries[idx].text);

        if(selected) canvas_set_color(c, ColorBlack);
    }

    if(m->count > TOC_VISIBLE) {
        int16_t bar_h = (int16_t)((uint32_t)TOC_VISIBLE * 53u / m->count);
        if(bar_h < 4) bar_h = 4;
        int16_t bar_y = TOC_HEADER_H;
        if(m->count > TOC_VISIBLE) {
            bar_y += (int16_t)((uint32_t)m->scroll_top * (53u - bar_h) /
                               (m->count - TOC_VISIBLE));
        }
        canvas_draw_box(c, 126, bar_y, 2, bar_h);
    }
}

static bool input_callback(InputEvent* evt, void* ctx) {
    TocView* v = ctx;
    bool consumed = false;
    bool fire_select = false;
    uint16_t selected = 0;

    if(evt->type != InputTypeShort && evt->type != InputTypeRepeat) return false;

    with_view_model(
        v->view,
        TocModel * m,
        {
            switch(evt->key) {
            case InputKeyUp:
                if(m->selected > 0) {
                    m->selected--;
                    toc_reset_text_scroll(m);
                }
                consumed = true;
                break;
            case InputKeyDown:
                if(m->selected + 1 < m->count) {
                    m->selected++;
                    toc_reset_text_scroll(m);
                }
                consumed = true;
                break;
            case InputKeyLeft:
                if(m->text_scroll_px > 8) {
                    m->text_scroll_px -= 8;
                } else {
                    m->text_scroll_px = 0;
                }
                consumed = true;
                break;
            case InputKeyRight:
                if(m->text_scroll_max_px == 0 && m->selected < m->count) {
                    m->text_scroll_max_px = toc_scroll_estimate(m->entries[m->selected].text);
                }
                if(m->text_scroll_px + 8 < m->text_scroll_max_px) {
                    m->text_scroll_px += 8;
                } else {
                    m->text_scroll_px = m->text_scroll_max_px;
                }
                consumed = true;
                break;
            case InputKeyOk:
                if(m->count > 0) {
                    selected = m->selected;
                    fire_select = true;
                }
                consumed = true;
                break;
            default:
                break;
            }
        },
        true);

    if(fire_select && v->cb) {
        v->cb(selected, v->cb_ctx);
    }
    return consumed;
}

static void scroll_timer_cb(void* ctx) {
    TocView* v = ctx;
    with_view_model(
        v->view,
        TocModel * m,
        {
            if(m->count > 0 && m->text_scroll_max_px > 0) {
                if(m->text_scroll_px + 2 < m->text_scroll_max_px + 12) {
                    m->text_scroll_px += 2;
                } else {
                    m->text_scroll_px = 0;
                }
            }
        },
        true);
}

TocView* toc_view_alloc(void) {
    TocView* v = malloc(sizeof(TocView));
    memset(v, 0, sizeof(*v));
    v->view = view_alloc();
    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(TocModel));
    with_view_model(
        v->view, TocModel * m, { memset(m, 0, sizeof(*m)); }, false);
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, render_callback);
    view_set_input_callback(v->view, input_callback);
    v->scroll_timer = furi_timer_alloc(scroll_timer_cb, FuriTimerTypePeriodic, v);
    furi_timer_start(v->scroll_timer, furi_ms_to_ticks(250));
    return v;
}

void toc_view_free(TocView* v) {
    if(!v) return;
    furi_timer_stop(v->scroll_timer);
    furi_timer_free(v->scroll_timer);
    view_free(v->view);
    free(v);
}

View* toc_view_get_view(TocView* v) {
    return v->view;
}

void toc_view_reset(TocView* v, const char* header, const char* empty_text) {
    with_view_model(
        v->view,
        TocModel * m,
        {
            memset(m, 0, sizeof(*m));
            if(header) strncpy(m->header, header, sizeof(m->header) - 1);
            if(empty_text) strncpy(m->empty_text, empty_text, sizeof(m->empty_text) - 1);
        },
        true);
}

void toc_view_add_entry(TocView* v, const char* text) {
    with_view_model(
        v->view,
        TocModel * m,
        {
            if(m->count < FBOOK_MAX_CHAPTERS) {
                strncpy(m->entries[m->count].text, text, TOC_LABEL_LEN - 1);
                m->entries[m->count].text[TOC_LABEL_LEN - 1] = '\0';
                m->count++;
                toc_reset_text_scroll(m);
            }
        },
        true);
}

void toc_view_set_select_callback(TocView* v, TocViewSelectCb cb, void* ctx) {
    v->cb = cb;
    v->cb_ctx = ctx;
}
