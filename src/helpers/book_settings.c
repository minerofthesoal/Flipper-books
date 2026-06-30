#include "book_settings.h"
#include "../books_app.h"
#include <storage/storage.h>
#include <furi.h>
#include <furi_hal_rtc.h>
#include <string.h>

#define SETTINGS_MAGIC 0x424F4B32u /* 'BOK2' - bumped for new fields */
#define STATS_MAGIC    0x53544132u /* 'STA2' */

void book_settings_set_defaults(BookSettings* s) {
    memset(s, 0, sizeof(*s));
    s->power_mode = PowerModeBalanced;
    s->page_animation = PageAnimSlide;
    s->text_size = TextSizeSmall;
    s->show_images = true;
    s->night_mode = false;
    s->auto_scroll = false;
    s->auto_scroll_speed = 6;
    s->backlight_level = 60;
    s->justify_text = false;
    s->hyphenate = true;
    s->show_progress_bar = true;
    s->vibrate_page_turn = false;

    s->line_spacing = LineSpacingNormal;
    s->font_family = FontFamilyDefault;
    s->margin = MarginNormal;
    s->show_page_number = true;
    s->show_clock = false;
    s->sleep_timer_minutes = 0;
    s->library_sort = SortModeRecent;
    s->show_covers_in_library = true;
}

static bool write_all(Storage* storage, const char* path, const void* data, size_t len) {
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = storage_file_write(f, data, len) == len;
        storage_file_close(f);
    }
    storage_file_free(f);
    return ok;
}

static bool read_all(Storage* storage, const char* path, void* data, size_t len) {
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        ok = storage_file_read(f, data, len) == len;
        storage_file_close(f);
    }
    storage_file_free(f);
    return ok;
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    BookSettings data;
} SettingsBlob;

bool book_settings_load(BookSettings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    SettingsBlob b;
    bool ok = read_all(storage, BOOKS_SETTINGS_FILE, &b, sizeof(b));
    if(ok && b.magic == SETTINGS_MAGIC) {
        *s = b.data;
    } else {
        book_settings_set_defaults(s);
        ok = false;
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool book_settings_save(const BookSettings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BOOKS_APP_FOLDER);
    SettingsBlob b = {.magic = SETTINGS_MAGIC, .version = 2, .data = *s};
    bool ok = write_all(storage, BOOKS_SETTINGS_FILE, &b, sizeof(b));
    furi_record_close(RECORD_STORAGE);
    return ok;
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    BookStats data;
} StatsBlob;

void book_stats_set_defaults(BookStats* s) {
    memset(s, 0, sizeof(*s));
}

bool book_stats_load(BookStats* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    StatsBlob b;
    bool ok = read_all(storage, BOOKS_STATS_FILE, &b, sizeof(b));
    if(ok && b.magic == STATS_MAGIC) {
        *s = b.data;
    } else {
        book_stats_set_defaults(s);
        ok = false;
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool book_stats_save(const BookStats* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BOOKS_APP_FOLDER);
    StatsBlob b = {.magic = STATS_MAGIC, .version = 2, .data = *s};
    bool ok = write_all(storage, BOOKS_STATS_FILE, &b, sizeof(b));
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* Days since 1970-01-01 for a proleptic Gregorian (y, m, d), per Howard
 * Hinnant's well-known civil_from_days/days_from_civil algorithm. Only used
 * to compare consecutive calendar days, so we don't need anything fancier. */
static uint32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2) ? 1 : 0;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);              // [0, 399]
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   // [0, 146096]
    return (uint32_t)(era * 146097 + (int32_t)doe + 719468);
}

void book_stats_update_streak(BookStats* s) {
    FuriHalRtcDateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    if(dt.year == 0) return; // RTC not set; leave streak as-is rather than guess

    uint32_t today = days_from_civil(dt.year, dt.month, dt.day);

    if(s->last_active_day == 0) {
        s->streak_days = 1;
    } else if(today == s->last_active_day) {
        // already counted today
    } else if(today == s->last_active_day + 1) {
        s->streak_days++;
    } else {
        // skipped a day (or the clock moved backward) - streak resets
        s->streak_days = 1;
    }
    s->last_active_day = today;
}

const char* power_mode_name(PowerMode m) {
    switch(m) {
    case PowerModePowerSaver: return "Saver";
    case PowerModeBalanced:   return "Balanced";
    case PowerModeGraphics:   return "Graphics";
    }
    return "?";
}

const char* page_anim_name(PageAnimation a) {
    switch(a) {
    case PageAnimNone:  return "None";
    case PageAnimSlide: return "Slide";
    case PageAnimFade:  return "Fade";
    case PageAnimCurl:  return "Curl";
    }
    return "?";
}

const char* text_size_name(TextSize t) {
    switch(t) {
    case TextSizeTiny:   return "Tiny";
    case TextSizeSmall:  return "Small";
    case TextSizeMedium: return "Medium";
    case TextSizeLarge:  return "Large";
    }
    return "?";
}

uint8_t text_size_pixels(TextSize t) {
    switch(t) {
    case TextSizeTiny:   return 5;
    case TextSizeSmall:  return 7;
    case TextSizeMedium: return 9;
    case TextSizeLarge:  return 12;
    }
    return 7;
}

const char* font_family_name(FontFamily f) {
    switch(f) {
    case FontFamilyDefault: return "Auto";
    case FontFamilySerif:   return "Serif";
    case FontFamilySans:    return "Sans";
    }
    return "?";
}

const char* line_spacing_name(LineSpacing l) {
    switch(l) {
    case LineSpacingTight:  return "Tight";
    case LineSpacingNormal: return "Normal";
    case LineSpacingLoose:  return "Loose";
    case LineSpacingDouble: return "Double";
    }
    return "?";
}

const char* margin_name(MarginSize m) {
    switch(m) {
    case MarginCompact: return "None";
    case MarginNormal:  return "Normal";
    case MarginWide:    return "Wide";
    }
    return "?";
}

const char* library_sort_name(LibrarySort s) {
    switch(s) {
    case SortModeName:           return "Name";
    case SortModeRecent:         return "Recent";
    case SortModeProgress:       return "Progress";
    case SortModeFavoritesFirst: return "Stars";
    }
    return "?";
}
