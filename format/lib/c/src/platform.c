/* platform.c: the platforms of spec Appendix A, as a reader needs them. MIT License. */
#include "internal.h"

static const char *const SWITCH_ARCHS[] = {"arm64"};
static const char *const N64_ARCHS[] = {"mips"};
static const char *const PC_ARCHS[] = {"x86-64", "arm64"};

static const char *const SWITCH_MODULES[] = {"main",    "sdk",     "subsdk0", "subsdk1", "subsdk2", "subsdk3",
                                             "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9"};
static const char *const N64_MODULES[] = {"rom"};

static const char *const SWITCH_REGIONS[] = {"heap", "alias", "aslr"};

static const char *const SWITCH_BUTTONS[] = {
    "A",           "B",           "X",           "Y",           "L",         "R",           "ZL",
    "ZR",          "PLUS",        "MINUS",       "LSTICK",      "RSTICK",    "DUP",         "DDOWN",
    "DLEFT",       "DRIGHT",      "LSTICK_UP",   "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT", "RSTICK_UP",
    "RSTICK_DOWN", "RSTICK_LEFT", "RSTICK_RIGHT", "SL",         "SR"};
static const char *const SWITCH2_BUTTONS[] = {
    "A",           "B",           "X",           "Y",           "L",         "R",           "ZL",
    "ZR",          "PLUS",        "MINUS",       "LSTICK",      "RSTICK",    "DUP",         "DDOWN",
    "DLEFT",       "DRIGHT",      "LSTICK_UP",   "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT", "RSTICK_UP",
    "RSTICK_DOWN", "RSTICK_LEFT", "RSTICK_RIGHT", "SL",         "SR",        "C"};
static const char *const N64_BUTTONS[] = {"A",    "B",     "Z",     "START", "L",     "R",     "CUP",
                                          "CDOWN", "CLEFT", "CRIGHT", "DUP",  "DDOWN", "DLEFT", "DRIGHT"};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const mn_platform PLATFORMS[] = {
    {"switch", 0, SWITCH_ARCHS, COUNT(SWITCH_ARCHS), 0, 8, SWITCH_MODULES, COUNT(SWITCH_MODULES), 0,
     SWITCH_REGIONS, COUNT(SWITCH_REGIONS), 0, SWITCH_BUTTONS, COUNT(SWITCH_BUTTONS)},
    {"switch2", 0, SWITCH_ARCHS, COUNT(SWITCH_ARCHS), 0, 8, SWITCH_MODULES, COUNT(SWITCH_MODULES), 0,
     SWITCH_REGIONS, COUNT(SWITCH_REGIONS), 0, SWITCH2_BUTTONS, COUNT(SWITCH2_BUTTONS)},
    {"n64", 1, N64_ARCHS, COUNT(N64_ARCHS), 1, 4, N64_MODULES, COUNT(N64_MODULES), 1, NULL, 0, 1, N64_BUTTONS,
     COUNT(N64_BUTTONS)},
    {"pc", 2, PC_ARCHS, COUNT(PC_ARCHS), 0, 8, NULL, 0, 1, NULL, 0, 0, NULL, 0},
};

const mn_platform *mn_platform_find(const char *name, size_t n)
{
    size_t i;
    for (i = 0; i < COUNT(PLATFORMS); i++) {
        if (mn_streqz(name, n, PLATFORMS[i].name)) {
            return &PLATFORMS[i];
        }
    }
    return NULL;
}

static int all_hex(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!mn_is_hex((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

int mn_platform_game_id(const mn_platform *p, const char *s, size_t n)
{
    size_t i;
    if (p->game_id_kind == 0) {
        return n == 16 && all_hex(s, n);
    }
    if (p->game_id_kind == 1) {
        if (n != 4) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x21 || c > 0x7E) {
                return 0;
            }
        }
        return 1;
    }
    /* <store>:<id>: [a-z0-9-]+ then a colon, then characters that are not white space. */
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ':') {
            break;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return 0;
        }
    }
    if (i == 0 || i == n || i + 1 == n) {
        return 0;
    }
    i++;
    while (i < n) {
        uint32_t cp;
        i += mn_utf8_decode((const uint8_t *)s + i, n - i, &cp);
        if (mn_py_space(cp)) {
            return 0;
        }
    }
    return 1;
}

int mn_platform_module_id(const mn_platform *p, const char *s, size_t n)
{
    if (!p->module_sha256) {
        return n >= 1 && n <= 64 && all_hex(s, n);
    }
    return n == 7 + 64 && memcmp(s, "sha256:", 7) == 0 && all_hex(s + 7, 64);
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

int mn_platform_module_match(const mn_platform *p, const char *listed, size_t ln, const char *running, size_t rn)
{
    size_t i, n;
    if (!p->module_sha256) {
        n = ln < rn ? ln : rn;
    } else {
        if (ln != rn) {
            return 0;
        }
        n = ln;
    }
    for (i = 0; i < n; i++) {
        if (lower((unsigned char)listed[i]) != lower((unsigned char)running[i])) {
            return 0;
        }
    }
    return 1;
}

int mn_platform_button(const mn_platform *p, const char *s, size_t n)
{
    size_t i;
    if (p->buttons != NULL) {
        for (i = 0; i < p->button_count; i++) {
            if (mn_streqz(s, n, p->buttons[i])) {
                return 1;
            }
        }
        return 0;
    }
    if (n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

int mn_platform_has_module(const mn_platform *p, const char *s, size_t n)
{
    size_t i;
    if (p->modules == NULL) {
        return 1;
    }
    for (i = 0; i < p->module_count; i++) {
        if (mn_streqz(s, n, p->modules[i])) {
            return 1;
        }
    }
    return 0;
}
