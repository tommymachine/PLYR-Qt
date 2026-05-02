// eq_presets.h — built-in preset table for Concerto's EQ.
//
// Read-only. The Qt/QML layer enumerates these to populate the preset
// picker and calls eq_apply_target() with the selected entry's values.

#pragma once

#include "eq_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* id;                              // stable machine id
    const char* display_name;                    // human label for UI
    double      band_gains_db[EQ_NUM_BANDS];
    double      preamp_db;
} EqPreset;

int             eq_preset_count(void);
const EqPreset* eq_preset_at   (int index);
const EqPreset* eq_preset_by_id(const char* id);

#ifdef __cplusplus
}
#endif
