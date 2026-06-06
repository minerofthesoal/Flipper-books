#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_input.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>

#include "scenes/books_scene.h"
#include "views/reader_view.h"
#include "views/library_view.h"
#include "views/toc_view.h"
#include "helpers/book_storage.h"
#include "helpers/book_settings.h"
#include "helpers/book_progress.h"

#define BOOKS_APP_FOLDER    "/ext/apps_data/books"
#define BOOKS_LIBRARY       "/ext/apps_data/books/library"
#define BOOKS_CACHE         "/ext/apps_data/books/cache"
#define BOOKS_PROGRESS      "/ext/apps_data/books/progress"
#define BOOKS_BOOKMARKS     "/ext/apps_data/books/bookmarks"
#define BOOKS_SETTINGS_FILE "/ext/apps_data/books/settings.cfg"
#define BOOKS_STATS_FILE    "/ext/apps_data/books/stats.cfg"

#define BOOKS_EXT_EPUB ".epub"
#define BOOKS_EXT_FBOOK ".fbook"
#define BOOKS_EXT_TXT  ".txt"

typedef enum {
    BooksViewSubmenu,
    BooksViewVarList,
    BooksViewTextInput,
    BooksViewDialog,
    BooksViewPopup,
    BooksViewReader,
    BooksViewLibrary,
    BooksViewToc,
} BooksView;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    Storage* storage;
    DialogsApp* dialogs;

    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    VariableItemList* var_list;
    TextInput* text_input;
    DialogEx* dialog;
    Popup* popup;
    ReaderView* reader;
    LibraryView* library;
    TocView* toc;

    BookSettings settings;
    BookStats stats;

    char current_book_path[256];
    char current_book_name[64];
    BookProgress progress;

    char text_input_buf[64];

    /* Pending value for the goto-percent picker, read by the scene's
     * on_event handler after the user confirms. */
    uint8_t goto_percent_pending;

    bool library_delete_mode;
    char pending_delete_path[256];
    char pending_delete_name[64];

    uint32_t session_start_tick;
} BooksApp;

BooksApp* books_app_alloc(void);
void books_app_free(BooksApp* app);
