#include "eq_presets.h"

#include <string.h>

// Values agreed with the user — see docs/EQ_RESEARCH.md §5.
//
// Bands (Hz): 31.5, 63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k
//
// Classical:  Winamp's documented values (gentle treble rolloff only).
// Jazz/Modern/Vinyl: designed for Concerto; see research §5.3–5.4.
static const EqPreset PRESETS[] = {
    {
        .id            = "flat",
        .display_name  = "Flat",
        .band_gains_db = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        .preamp_db     = 0.0,
    },
    {
        .id            = "classical",
        .display_name  = "Classical",
        .band_gains_db = { 0, 0, 0, 0, 0, 0, -7.2, -7.2, -7.2, -9.6 },
        .preamp_db     = 0.0,
    },
    {
        .id            = "jazz",
        .display_name  = "Jazz",
        .band_gains_db = { +3, +2, +1, +2, -1, -1,  0, +1, +2, +3 },
        .preamp_db     = 0.0,
    },
    {
        .id            = "modern",
        .display_name  = "Modern",
        .band_gains_db = { +2, +3, +1,  0, -1,  0, +2, +3, +4, +4 },
        .preamp_db     = -2.0,
    },
    {
        .id            = "vinyl",
        .display_name  = "Vinyl",
        .band_gains_db = { +3, +3, +2, +2, +1, -1, -2, -3, -4, -5 },
        .preamp_db     = -2.0,
    },
};

int eq_preset_count(void) {
    return (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
}

const EqPreset* eq_preset_at(int index) {
    if (index < 0 || index >= eq_preset_count()) return NULL;
    return &PRESETS[index];
}

const EqPreset* eq_preset_by_id(const char* id) {
    if (!id) return NULL;
    int n = eq_preset_count();
    for (int i = 0; i < n; i++)
        if (strcmp(PRESETS[i].id, id) == 0) return &PRESETS[i];
    return NULL;
}
