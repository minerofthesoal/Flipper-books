#include "../books_app.h"
#include <stdio.h>

static FBook* cached_book;
static uint32_t cached_pages[FBOOK_MAX_CHAPTERS];

static void toc_select_cb(uint16_t index, void* ctx) {
    BooksApp* app = ctx;
    if(!cached_book || index >= cached_book->chapter_count) return;
    app->progress.offset = cached_book->chapters[index].offset;
    app->progress.page = cached_pages[index];
    book_progress_save(app->current_book_path, &app->progress);
    view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventJumpTo);
}

void books_scene_toc_on_enter(void* ctx) {
    BooksApp* app = ctx;
    TocView* toc = app->toc;
    toc_view_reset(toc, "Contents", "(no chapters)");
    toc_view_set_select_callback(toc, toc_select_cb, app);

    cached_book = fbook_alloc();
    if(!fbook_open(cached_book, app->current_book_path)) {
        toc_view_reset(toc, "Contents", "(no TOC)");
        fbook_free(cached_book);
        cached_book = NULL;
    } else if(cached_book->chapter_count == 0) {
        toc_view_reset(toc, "Contents", "(no chapters)");
    } else {
        if(cached_book->flags & FBOOK2_FLAG_CHAPTER_PAGES) {
            for(uint16_t i = 0; i < cached_book->chapter_count; ++i) {
                cached_pages[i] = cached_book->chapters[i].page;
            }
        } else {
            uint32_t offsets[FBOOK_MAX_CHAPTERS];
            for(uint16_t i = 0; i < cached_book->chapter_count; ++i) {
                offsets[i] = cached_book->chapters[i].offset;
            }
            reader_view_build_page_map(
                cached_book,
                &app->settings,
                offsets,
                cached_book->chapter_count,
                cached_pages);
        }

        for(uint16_t i = 0; i < cached_book->chapter_count; ++i) {
            const char* title = cached_book->chapters[i].title;
            char label[96];
            snprintf(
                label,
                sizeof(label),
                "%s  p%lu",
                title[0] ? title : "(untitled)",
                (unsigned long)(cached_pages[i] + 1));
            toc_view_add_entry(toc, label);
        }
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, BooksViewToc);
}

bool books_scene_toc_on_event(void* ctx, SceneManagerEvent event) {
    BooksApp* app = ctx;
    if(event.type == SceneManagerEventTypeCustom && event.event == BooksEventJumpTo) {
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, BooksSceneReader);
        return true;
    }
    return false;
}

void books_scene_toc_on_exit(void* ctx) {
    UNUSED(ctx);
    if(cached_book) {
        fbook_free(cached_book);
        cached_book = NULL;
    }
}
