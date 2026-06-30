#include "../books_app.h"
#include <string.h>
#include <strings.h>

static FBook* g_book = NULL;

static void open_fail_popup_cb(void* ctx) {
    BooksApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventCloseBook);
}

static void reader_event_cb(ReaderPublicEvent ev, void* ctx) {
    BooksApp* app = ctx;
    switch(ev) {
    case ReaderEventBack:
        view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventCloseBook);
        break;
    case ReaderEventMenu:
        view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventOpenMenu);
        break;
    case ReaderEventBookmark:
        view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventToggleBookmark);
        break;
    case ReaderEventToc:
        view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventShowTOC);
        break;
    case ReaderEventSearch:
        view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventShowSearch);
        break;
    case ReaderEventSearchNext:
        view_dispatcher_send_custom_event(app->view_dispatcher, BooksEventSearchNext);
        break;
    }
}

void books_scene_reader_on_enter(void* ctx) {
    BooksApp* app = ctx;

    if(!g_book) {
        g_book = fbook_alloc();
    }
    if(!g_book || !fbook_open(g_book, app->current_book_path)) {
        // failed to open - show popup that returns to the library after a tap or timeout
        popup_reset(app->popup);
        const char* dot = strrchr(app->current_book_path, '.');
        bool is_epub = dot && strcasecmp(dot, BOOKS_EXT_EPUB) == 0;
        bool is_fbook = dot && strcasecmp(dot, BOOKS_EXT_FBOOK) == 0;
        popup_set_header(app->popup, "Cannot open", 64, 8, AlignCenter, AlignTop);
        const char* msg;
        if(is_epub) {
            msg = "Run epub_to_fbook.py\nand drop the .fbook in";
        } else if(is_fbook) {
            /* Likely an empty/old .fbook from the buggy converter run. */
            msg = "Empty .fbook -\nre-run the converter";
        } else {
            msg = "File is missing or\nnot a valid book";
        }
        popup_set_text(app->popup, msg, 64, 28, AlignCenter, AlignTop);
        popup_set_context(app->popup, app);
        popup_set_callback(app->popup, open_fail_popup_cb);
        popup_set_timeout(app->popup, 3000);
        popup_enable_timeout(app->popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, BooksViewPopup);
        fbook_free(g_book);
        g_book = NULL;
        return;
    }

    book_progress_load(app->current_book_path, &app->progress);
    app->progress.total_bytes = g_book->text_length;

    reader_view_resume(app->reader);
    reader_view_set_settings(app->reader, &app->settings);
    reader_view_set_book(app->reader, g_book);
    reader_view_set_progress(app->reader, &app->progress);
    reader_view_set_event_callback(app->reader, reader_event_cb, app);
    reader_view_set_notifications(app->reader, app->notifications);

    app->stats.books_opened++;
    view_dispatcher_switch_to_view(app->view_dispatcher, BooksViewReader);
}

static void save_progress(BooksApp* app) {
    if(!g_book) return;
    uint32_t prev_offset = app->progress.offset;
    app->progress.offset = reader_view_get_offset(app->reader);
    app->progress.page = reader_view_get_page(app->reader);
    app->progress.total_bytes = g_book->text_length;
    app->progress.last_read = furi_get_tick();
    app->progress.last_chapter = fbook_find_chapter(g_book, app->progress.offset);
    app->progress.words_in_book = g_book->word_count;
    app->progress.pages_turned++;

    /* Count just the new words covered since the last save into the global
     * stats so reading speed is measured against material we're sure was
     * read forward (skip backwards/jumps). */
    if(app->progress.offset > prev_offset && g_book->text_length > 0 &&
       g_book->word_count > 0) {
        uint32_t delta_bytes = app->progress.offset - prev_offset;
        uint32_t delta_words =
            (uint32_t)((uint64_t)delta_bytes * g_book->word_count /
                       g_book->text_length);
        app->stats.total_words_read += delta_words;
    }

    if(!app->progress.finished &&
       g_book->text_length > 0 &&
       app->progress.offset >= g_book->text_length - 16) {
        app->progress.finished = 1;
        app->stats.books_finished++;
    }

    book_progress_save(app->current_book_path, &app->progress);
    app->stats.total_pages_read++;
}

bool books_scene_reader_on_event(void* ctx, SceneManagerEvent event) {
    BooksApp* app = ctx;
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case BooksEventCloseBook:
        save_progress(app);
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, BooksSceneStart);
        return true;
    case BooksEventOpenMenu:
        save_progress(app);
        scene_manager_next_scene(app->scene_manager, BooksSceneMenu);
        return true;
    case BooksEventToggleBookmark:
        save_progress(app);
        scene_manager_next_scene(app->scene_manager, BooksSceneAddBookmark);
        return true;
    case BooksEventShowTOC:
        save_progress(app);
        scene_manager_next_scene(app->scene_manager, BooksSceneTOC);
        return true;
    case BooksEventSearchNext: {
        if(!g_book) return true;
        uint32_t off = reader_view_get_offset(app->reader);
        uint32_t found = fbook_search(g_book, off + 1, app->text_input_buf);
        if(found != UINT32_MAX) reader_view_jump_to(app->reader, found);
        return true;
    }
    case BooksEventJumpTo: {
        reader_view_jump_to(app->reader, app->progress.offset);
        return true;
    }
    }
    return false;
}

void books_scene_reader_on_exit(void* ctx) {
    BooksApp* app = ctx;
    /* Stop the view's background timers and drop its FBook* BEFORE freeing
     * g_book below. Leaving the view's model pointing at freed memory while
     * its timers keep running in the background (page-turn animation tick,
     * and especially auto-scroll) is a use-after-free: it can corrupt the
     * heap silently, which is what produced the "freeze + force-restart"
     * hangs and the intermittent "out of memory" failures on the next book
     * opened, since the next allocation can land on the trashed heap. */
    reader_view_pause(app->reader);
    reader_view_clear_book(app->reader);
    if(g_book) {
        save_progress(app);
        fbook_free(g_book);
        g_book = NULL;
    }
}
