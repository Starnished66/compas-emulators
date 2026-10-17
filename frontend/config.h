#ifndef FRONTEND_CONFIG_H
#define FRONTEND_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-emulator frontend settings, read from the "frontend" object of the
 * emulator's emulator.json (docs/PROTOCOL.md leaves unknown keys to the
 * emulator):
 *
 *   "frontend": { "buttons": ["b", "a", "select", "start"],
 *                 "options": { "core_option_key": "value" },
 *                 "frameskip": "auto",
 *                 "skip_options": { "core_option_key": "value" },
 *                 "audio_stretch": false }
 *
 * "buttons" lists the libretro joypad buttons that get on-screen controls
 * (a, b, x, y, l, r, select, start; the D-pad is always present).
 * "options" are the answers to RETRO_ENVIRONMENT_GET_VARIABLE.
 * "frameskip" is "off" (the default), "auto" (the video of the frame after
 * one that ran over its time budget is skipped, never two in a row) or "half"
 * (every second frame's video is skipped). Audio is never skipped. The core is
 * told through RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE; "skip_options"
 * are the option answers that replace "options" while a frame's video is
 * skipped, for a core that cannot skip rendering by itself but has options
 * that turn parts of it off.
 * "audio_stretch" (true or false, default false): when the emulator falls
 * behind real time the audio is stretched (played slower and lower) so the
 * device buffer never runs dry; see stretch.h. Nothing changes while the
 * emulator keeps up. */

#define FRONTEND_MAX_OPTIONS 32
#define FRONTEND_OPTION_TEXT 64

typedef struct {
    char key[FRONTEND_OPTION_TEXT];
    char value[FRONTEND_OPTION_TEXT];
} frontend_option_t;

typedef enum {
    FRAMESKIP_OFF,
    FRAMESKIP_AUTO,
    FRAMESKIP_HALF,
} frameskip_mode_t;

typedef struct {
    uint32_t buttons; /* bit n set: RETRO_DEVICE_ID_JOYPAD_<n> has a control */
    int option_count;
    frontend_option_t options[FRONTEND_MAX_OPTIONS];
    frameskip_mode_t frameskip;
    bool audio_stretch;
    int skip_option_count;
    frontend_option_t skip_options[FRONTEND_MAX_OPTIONS];
} frontend_config_t;

/* The NES layout: B, A, SELECT, START and no options. */
void frontend_config_default(frontend_config_t * cfg);

/* Parses the manifest text. On any problem returns false, leaves *cfg at the
 * defaults and writes a short reason to error. A manifest without a
 * "frontend" object is fine and gives the defaults. */
bool frontend_config_parse(const char * json, size_t length, frontend_config_t * cfg, char * error, size_t error_size);

/* Value of a core option, or NULL when the manifest does not set it. */
const char * frontend_config_option(const frontend_config_t * cfg, const char * key);

/* Value of a core option while a frame's video is skipped, or NULL. */
const char * frontend_config_skip_option(const frontend_config_t * cfg, const char * key);

/* Sets (or replaces) a core option answer, or with skip an answer used while video is skipped;
 * false when the table is full. */
bool frontend_config_set_option(frontend_config_t * cfg, const char * key, const char * value, bool skip);

/* "off", "auto" or "half"; false for anything else. */
bool frontend_config_parse_frameskip(const char * text, frameskip_mode_t * mode);

#endif /* FRONTEND_CONFIG_H */
