#include "books_app.h"

#include <furi.h>
#include <notification/notification_messages.h>
#include <stdlib.h>

static bool books_custom_event_callback(void* ctx, uint32_t event) {
    BooksApp* app = ctx;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool books_back_event_callback(void* ctx) {
    BooksApp* app = ctx;
    return scene_manager_handle_back_event(app->scene_manager);
}

BooksApp* books_app_alloc(void) {
    BooksApp* app = malloc(sizeof(BooksApp));
    memset(app, 0, sizeof(*app));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    storage_simply_mkdir(app->storage, BOOKS_APP_FOLDER);
    storage_simply_mkdir(app->storage, BOOKS_LIBRARY);
    storage_simply_mkdir(app->storage, BOOKS_CACHE);
    storage_simply_mkdir(app->storage, BOOKS_PROGRESS);
    storage_simply_mkdir(app->storage, BOOKS_BOOKMARKS);

    book_settings_load(&app->settings);
    book_stats_load(&app->stats);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&books_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, books_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, books_back_event_callback);

    app->submenu = submenu_alloc();
    app->var_list = variable_item_list_alloc();
    app->text_input = text_input_alloc();
    app->dialog = dialog_ex_alloc();
    app->popup = popup_alloc();
    app->reader = reader_view_alloc();
    app->library = library_view_alloc();
    app->toc = toc_view_alloc();

    view_dispatcher_add_view(app->view_dispatcher, BooksViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, BooksViewVarList, variable_item_list_get_view(app->var_list));
    view_dispatcher_add_view(app->view_dispatcher, BooksViewTextInput, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, BooksViewDialog, dialog_ex_get_view(app->dialog));
    view_dispatcher_add_view(app->view_dispatcher, BooksViewPopup, popup_get_view(app->popup));
    view_dispatcher_add_view(app->view_dispatcher, BooksViewReader, reader_view_get_view(app->reader));
    view_dispatcher_add_view(app->view_dispatcher, BooksViewLibrary, library_view_get_view(app->library));
    view_dispatcher_add_view(app->view_dispatcher, BooksViewToc, toc_view_get_view(app->toc));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->session_start_tick = furi_get_tick();

    return app;
}

void books_app_free(BooksApp* app) {
    if(!app) return;

    // persist stats
    uint32_t elapsed_s = (furi_get_tick() - app->session_start_tick) / furi_kernel_get_tick_frequency();
    app->stats.total_read_seconds += elapsed_s;
    book_stats_save(&app->stats);
    book_settings_save(&app->settings);

    view_dispatcher_remove_view(app->view_dispatcher, BooksViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, BooksViewVarList);
    view_dispatcher_remove_view(app->view_dispatcher, BooksViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, BooksViewDialog);
    view_dispatcher_remove_view(app->view_dispatcher, BooksViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, BooksViewReader);
    view_dispatcher_remove_view(app->view_dispatcher, BooksViewLibrary);
    view_dispatcher_remove_view(app->view_dispatcher, BooksViewToc);

    submenu_free(app->submenu);
    variable_item_list_free(app->var_list);
    text_input_free(app->text_input);
    dialog_ex_free(app->dialog);
    popup_free(app->popup);
    reader_view_free(app->reader);
    library_view_free(app->library);
    toc_view_free(app->toc);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t flipper_books_app(void* p) {
    UNUSED(p);
    BooksApp* app = books_app_alloc();

    scene_manager_next_scene(app->scene_manager, BooksSceneStart);

    // apply backlight according to power mode
    if(app->settings.power_mode == PowerModeGraphics) {
        notification_message(app->notifications, &sequence_display_backlight_enforce_on);
    } else if(app->settings.power_mode == PowerModePowerSaver) {
        /* Momentum has no _enforce_off; reader_view re-issues this on every
         * key press while in Power Saver so input events can't re-light it. */
        notification_message(app->notifications, &sequence_display_backlight_off);
    }

    view_dispatcher_run(app->view_dispatcher);

    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);

    books_app_free(app);
    return 0;
}
