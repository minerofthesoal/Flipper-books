#pragma once

#include <gui/view.h>
#include <stdint.h>

#include "../helpers/book_storage.h"

typedef void (*TocViewSelectCb)(uint16_t index, void* ctx);

typedef struct TocView TocView;

TocView* toc_view_alloc(void);
void toc_view_free(TocView* v);
View* toc_view_get_view(TocView* v);

void toc_view_reset(TocView* v, const char* header, const char* empty_text);
void toc_view_add_entry(TocView* v, const char* text);
void toc_view_set_select_callback(TocView* v, TocViewSelectCb cb, void* ctx);
