#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PowerModePowerSaver = 0,
    PowerModeBalanced = 1,
    PowerModeGraphics = 2,
} PowerMode;

typedef enum {
    PageAnimNone = 0,
    PageAnimSlide = 1,
    PageAnimFade = 2,
    PageAnimCurl = 3,
} PageAnimation;

typedef enum {
    TextSizeTiny = 0,
    TextSizeSmall = 1,
    TextSizeMedium = 2,
    TextSizeLarge = 3,
} TextSize;

typedef enum {
    FontFamilyDefault = 0, // FontPrimary for medium/large, FontSecondary for small/tiny
    FontFamilySerif = 1,   // FontPrimary always
    FontFamilySans = 2,    // FontSecondary always
} FontFamily;

typedef enum {
    LineSpacingTight = 0,  // -1px from default
    LineSpacingNormal = 1, // default
    LineSpacingLoose = 2,  // +1px
    LineSpacingDouble = 3, // +3px
} LineSpacing;

typedef enum {
    MarginCompact = 0, // 0px side margin
    MarginNormal = 1,  // 1px side margin
    MarginWide = 2,    // 4px side margin
} MarginSize;

typedef enum {
    SortModeName = 0,        // alphabetical
    SortModeRecent = 1,      // most recently read first
    SortModeProgress = 2,    // least progress first (in-progress books)
    SortModeFavoritesFirst = 3,
} LibrarySort;

typedef struct {
    PowerMode power_mode;
    PageAnimation page_animation;
    TextSize text_size;
    bool show_images;
    bool night_mode;
    bool auto_scroll;
    uint8_t auto_scroll_speed;   // 1..10 (seconds per page)
    uint8_t backlight_level;     // 0..100 (%)
    bool justify_text;
    bool hyphenate;
    bool show_progress_bar;
    bool vibrate_page_turn;

    // Added in v2:
    LineSpacing line_spacing;
    FontFamily font_family;
    MarginSize margin;
    bool show_page_number;       // overlay "p X / Y" on the bottom-right
    bool show_clock;             // small clock in the corner
    uint8_t sleep_timer_minutes; // 0 = off; 5/10/15/30/60
    LibrarySort library_sort;
    bool show_covers_in_library;
} BookSettings;

typedef struct {
    uint32_t total_read_seconds;
    uint32_t total_pages_read;
    uint32_t books_opened;
    uint32_t books_finished;
    uint32_t total_words_read;       // added in v2

    // Added in v3: day-based reading streak.
    uint32_t streak_days;            // consecutive calendar days the app was opened
    uint32_t last_active_day;        // days-since-epoch of the last streak update; 0 = never
} BookStats;

void book_settings_set_defaults(BookSettings* s);
bool book_settings_load(BookSettings* s);
bool book_settings_save(const BookSettings* s);

void book_stats_set_defaults(BookStats* s);
bool book_stats_load(BookStats* s);
bool book_stats_save(const BookStats* s);

/** Call once per app launch. Bumps streak_days if today is the day right
 *  after last_active_day, resets to 1 if a day (or more) was skipped, and
 *  leaves it alone if the app was already opened today. Uses the device RTC. */
void book_stats_update_streak(BookStats* s);

const char* power_mode_name(PowerMode m);
const char* page_anim_name(PageAnimation a);
const char* text_size_name(TextSize t);
uint8_t text_size_pixels(TextSize t);
const char* font_family_name(FontFamily f);
const char* line_spacing_name(LineSpacing l);
const char* margin_name(MarginSize m);
const char* library_sort_name(LibrarySort s);
