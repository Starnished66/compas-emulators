/* Minimal JSON reader for the "frontend" object of emulator.json. It
 * understands the whole JSON grammar well enough to skip every other key,
 * and keeps only what the frontend uses. */
#include "config.h"

#include "libretro.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char * p;
    const char * end;
    const char * error;
} reader_t;

#define MAX_DEPTH 16

static void skip_ws(reader_t * r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) r->p++;
}

static bool fail(reader_t * r, const char * why) {
    if (!r->error) r->error = why;
    return false;
}

static bool expect(reader_t * r, char c) {
    skip_ws(r);
    if (r->p < r->end && *r->p == c) {
        r->p++;
        return true;
    }
    return fail(r, "malformed JSON");
}

/* Reads a string; copies at most size-1 bytes into out (out may be NULL).
 * An escape other than the simple ones becomes '?'. */
static bool read_string(reader_t * r, char * out, size_t size) {
    skip_ws(r);
    if (r->p >= r->end || *r->p != '"') return fail(r, "expected a string");
    r->p++;
    size_t n = 0;
    while (r->p < r->end && *r->p != '"') {
        char c = *r->p++;
        if (c == '\\') {
            if (r->p >= r->end) break;
            char e = *r->p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '"': case '\\': case '/': c = e; break;
                case 'u':
                    for (int i = 0; i < 4 && r->p < r->end; i++) r->p++;
                    c = '?';
                    break;
                default: c = '?'; break;
            }
        }
        if (out && n + 1 < size) out[n++] = c;
    }
    if (r->p >= r->end) return fail(r, "unterminated string");
    r->p++;
    if (out && size) out[n] = '\0';
    return true;
}

static bool skip_value(reader_t * r, int depth);

static bool skip_container(reader_t * r, char close, bool object, int depth) {
    skip_ws(r);
    if (r->p < r->end && *r->p == close) {
        r->p++;
        return true;
    }
    for (;;) {
        if (object && (!read_string(r, NULL, 0) || !expect(r, ':'))) return false;
        if (!skip_value(r, depth + 1)) return false;
        skip_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        return expect(r, close);
    }
}

static bool skip_value(reader_t * r, int depth) {
    if (depth > MAX_DEPTH) return fail(r, "JSON nested too deeply");
    skip_ws(r);
    if (r->p >= r->end) return fail(r, "malformed JSON");
    switch (*r->p) {
        case '{': r->p++; return skip_container(r, '}', true, depth);
        case '[': r->p++; return skip_container(r, ']', false, depth);
        case '"': return read_string(r, NULL, 0);
        default: {
            const char * start = r->p;
            while (r->p < r->end && *r->p != ',' && *r->p != '}' && *r->p != ']' && *r->p != ' ' &&
                   *r->p != '\t' && *r->p != '\n' && *r->p != '\r') r->p++;
            return r->p > start ? true : fail(r, "malformed JSON");
        }
    }
}

/* A scalar as text: strings as they are, numbers and booleans verbatim. */
static bool read_scalar_text(reader_t * r, char * out, size_t size) {
    skip_ws(r);
    if (r->p < r->end && *r->p == '"') return read_string(r, out, size);
    if (r->p < r->end && (*r->p == '{' || *r->p == '[')) return fail(r, "option values must be text");
    const char * start = r->p;
    if (!skip_value(r, 0)) return false;
    size_t n = (size_t) (r->p - start);
    if (n >= size) n = size - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static int button_id(const char * name) {
    static const struct { const char * name; int id; } table[] = {
        { "a", RETRO_DEVICE_ID_JOYPAD_A },       { "b", RETRO_DEVICE_ID_JOYPAD_B },
        { "x", RETRO_DEVICE_ID_JOYPAD_X },       { "y", RETRO_DEVICE_ID_JOYPAD_Y },
        { "l", RETRO_DEVICE_ID_JOYPAD_L },       { "r", RETRO_DEVICE_ID_JOYPAD_R },
        { "select", RETRO_DEVICE_ID_JOYPAD_SELECT }, { "start", RETRO_DEVICE_ID_JOYPAD_START },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(name, table[i].name) == 0) return table[i].id;
    return -1;
}

static bool parse_buttons(reader_t * r, frontend_config_t * cfg) {
    if (!expect(r, '[')) return false;
    uint32_t mask = 0;
    skip_ws(r);
    if (r->p < r->end && *r->p == ']') return fail(r, "buttons is empty");
    for (;;) {
        char name[16];
        if (!read_string(r, name, sizeof(name))) return false;
        int id = button_id(name);
        if (id < 0) return fail(r, "unknown button name");
        mask |= 1u << id;
        skip_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        if (!expect(r, ']')) return false;
        break;
    }
    cfg->buttons = mask;
    return true;
}

static bool parse_options(reader_t * r, frontend_option_t * table, int * count) {
    if (!expect(r, '{')) return false;
    skip_ws(r);
    if (r->p < r->end && *r->p == '}') {
        r->p++;
        return true;
    }
    for (;;) {
        char key[FRONTEND_OPTION_TEXT], value[FRONTEND_OPTION_TEXT];
        if (!read_string(r, key, sizeof(key)) || !expect(r, ':') || !read_scalar_text(r, value, sizeof(value)))
            return false;
        if (*count >= FRONTEND_MAX_OPTIONS) return fail(r, "too many options");
        frontend_option_t * o = &table[(*count)++];
        snprintf(o->key, sizeof(o->key), "%s", key);
        snprintf(o->value, sizeof(o->value), "%s", value);
        skip_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        return expect(r, '}');
    }
}

static bool parse_frontend_object(reader_t * r, frontend_config_t * cfg) {
    if (!expect(r, '{')) return false;
    skip_ws(r);
    if (r->p < r->end && *r->p == '}') {
        r->p++;
        return true;
    }
    for (;;) {
        char key[32];
        if (!read_string(r, key, sizeof(key)) || !expect(r, ':')) return false;
        if (strcmp(key, "buttons") == 0) {
            if (!parse_buttons(r, cfg)) return false;
        } else if (strcmp(key, "options") == 0) {
            if (!parse_options(r, cfg->options, &cfg->option_count)) return false;
        } else if (strcmp(key, "skip_options") == 0) {
            if (!parse_options(r, cfg->skip_options, &cfg->skip_option_count)) return false;
        } else if (strcmp(key, "audio_stretch") == 0) {
            char flag[8];
            if (!read_scalar_text(r, flag, sizeof(flag))) return false;
            if (strcmp(flag, "true") == 0) cfg->audio_stretch = true;
            else if (strcmp(flag, "false") == 0) cfg->audio_stretch = false;
            else return fail(r, "audio_stretch must be true or false");
        } else if (strcmp(key, "frameskip") == 0) {
            char mode[16];
            if (!read_string(r, mode, sizeof(mode))) return false;
            if (!frontend_config_parse_frameskip(mode, &cfg->frameskip)) return fail(r, "unknown frameskip mode");
        } else if (!skip_value(r, 2)) {
            return false;
        }
        skip_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        return expect(r, '}');
    }
}

void frontend_config_default(frontend_config_t * cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->buttons = (1u << RETRO_DEVICE_ID_JOYPAD_B) | (1u << RETRO_DEVICE_ID_JOYPAD_A) |
                   (1u << RETRO_DEVICE_ID_JOYPAD_SELECT) | (1u << RETRO_DEVICE_ID_JOYPAD_START);
}

bool frontend_config_parse(const char * json, size_t length, frontend_config_t * cfg, char * error, size_t error_size) {
    frontend_config_default(cfg);
    frontend_config_t parsed;
    frontend_config_default(&parsed);
    reader_t r = { json, json + length, NULL };
    bool ok = expect(&r, '{');
    skip_ws(&r);
    if (ok && r.p < r.end && *r.p == '}') {
        r.p++;
    } else if (ok) {
        for (;;) {
            char key[32];
            if (!read_string(&r, key, sizeof(key)) || !expect(&r, ':')) { ok = false; break; }
            ok = strcmp(key, "frontend") == 0 ? parse_frontend_object(&r, &parsed) : skip_value(&r, 1);
            if (!ok) break;
            skip_ws(&r);
            if (r.p < r.end && *r.p == ',') {
                r.p++;
                continue;
            }
            ok = expect(&r, '}');
            break;
        }
    }
    if (!ok) {
        if (error && error_size) snprintf(error, error_size, "%s", r.error ? r.error : "malformed JSON");
        return false;
    }
    *cfg = parsed;
    if (error && error_size) error[0] = '\0';
    return true;
}

static const char * find_option(const frontend_option_t * table, int count, const char * key) {
    for (int i = 0; i < count; i++)
        if (strcmp(table[i].key, key) == 0) return table[i].value;
    return NULL;
}

const char * frontend_config_option(const frontend_config_t * cfg, const char * key) {
    return find_option(cfg->options, cfg->option_count, key);
}

const char * frontend_config_skip_option(const frontend_config_t * cfg, const char * key) {
    return find_option(cfg->skip_options, cfg->skip_option_count, key);
}

bool frontend_config_set_option(frontend_config_t * cfg, const char * key, const char * value, bool skip) {
    frontend_option_t * table = skip ? cfg->skip_options : cfg->options;
    int * count = skip ? &cfg->skip_option_count : &cfg->option_count;
    for (int i = 0; i < *count; i++)
        if (strcmp(table[i].key, key) == 0) {
            snprintf(table[i].value, sizeof(table[i].value), "%s", value);
            return true;
        }
    if (*count >= FRONTEND_MAX_OPTIONS) return false;
    frontend_option_t * o = &table[(*count)++];
    snprintf(o->key, sizeof(o->key), "%s", key);
    snprintf(o->value, sizeof(o->value), "%s", value);
    return true;
}

bool frontend_config_parse_frameskip(const char * text, frameskip_mode_t * mode) {
    static const struct { const char * name; frameskip_mode_t mode; } table[] = {
        { "off", FRAMESKIP_OFF }, { "auto", FRAMESKIP_AUTO }, { "half", FRAMESKIP_HALF },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(text, table[i].name) == 0) {
            *mode = table[i].mode;
            return true;
        }
    return false;
}
