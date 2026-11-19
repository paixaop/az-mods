/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint32_t cues_colors[9] = { 0xff00c6, 0xff0000, 0xff7100, 0xffff00, 0x00e300,
                                  0x00c3ff, 0x0051ff, 0x9c08ff, 0 };
const char *const cues_color_names[9] = { "Pink", "Red", "Orange", "Yellow", "Green",
                                          "Aqua", "Blue", "Purple", "No Color" };
const char *const cues_phrase_names[8] = { "Any", "Intro", "Build", "Break",
                                           "Chorus/Drop", "Outro", "Verse", "Bridge" };
static const char *const units[] = { "bars", "beats", "seconds" };
static const char *const quant[] = { "Inherit", "Off", "Beat", "Bar" };
static const char *const measures[] = { "Bars", "Beats", "Seconds" };
static const char *const anchors[] = { "start", "middle", "end" };
static int errorf(char *out, size_t n, const char *s)
{
    if (n)
        snprintf(out, n, "%s", s);
    return 0;
}
void cues_config_default(cues_config *c)
{
    memset(c, 0, sizeof *c);
    c->max_memory = 10;
    c->hot_pads = 8;
    c->quantization = CUES_QUANT_BEAT;
    c->min_bpm = 20;
    c->max_bpm = 400;
    c->min_distance = 0;
    c->cue_preference = AZ_CUE_HOT;
}
void cues_group_default(cues_group *g, const cues_config *c)
{
    memset(g, 0, sizeof *g);
    g->enabled = 1;
    g->measures_in = c->measures_in;
    snprintf(g->name, sizeof g->name, "New group");
}
void cues_rule_default(cues_rule *r, const cues_group *g)
{
    memset(r, 0, sizeof *r);
    r->enabled = 1;
    r->cue_type = AZ_CUE_HOT;
    r->pad = 1;
    r->phrase_type = 1;
    r->unit = r->offset_unit = r->loop_unit = g->measures_in;
    r->loop_length = 8;
    r->color = 1;
    snprintf(r->name, sizeof r->name, "{phrase}");
}
static int range(int v, int a, int b)
{
    return v >= a && v <= b;
}
static int num(double v, double a, double b)
{
    return isfinite(v) && v >= a && v <= b;
}
static int terminated(const char *s, size_t n)
{
    return memchr(s, 0, n) != NULL;
}
static int cue_type_valid(int type)
{
    return type == AZ_CUE_HOT || type == AZ_CUE_MEMORY;
}
static int config_limits_valid(const cues_config *c)
{
    return range(c->enabled, 0, 1) && range(c->max_memory, 0, 10) && range(c->hot_pads, 1, 8) &&
           range(c->quantization, 1, 3) && range(c->measures_in, 0, 2) && cue_type_valid(c->cue_preference);
}
static int config_valid(const cues_config *c)
{
    return c && config_limits_valid(c) && num(c->min_bpm, 1, 500) && num(c->max_bpm, c->min_bpm, 500) &&
           num(c->min_distance, 0, 3600) && range(c->group_count, 0, CUES_MAX_GROUPS);
}
static int group_text_valid(const cues_group *g)
{
    return terminated(g->name, sizeof g->name) && g->name[0] && terminated(g->comment, sizeof g->comment) &&
           terminated(g->artist, sizeof g->artist) && terminated(g->key, sizeof g->key);
}
static int group_bpm_valid(const cues_group *g, const cues_config *c)
{
    double minimum = g->min_bpm > 0 ? g->min_bpm : c->min_bpm;
    double maximum = g->max_bpm > 0 ? g->max_bpm : c->max_bpm;
    return num(g->min_bpm, 0, 500) && num(g->max_bpm, 0, 500) && minimum <= maximum;
}
static int group_valid(const cues_group *g, const cues_config *c)
{
    return range(g->enabled, 0, 1) && range(g->quantization, 0, 3) && range(g->measures_in, 0, 2) &&
           range(g->rule_count, 0, CUES_MAX_RULES) && group_bpm_valid(g, c) && group_text_valid(g);
}
static int rule_target_valid(const cues_rule *r)
{
    return range(r->enabled, 0, 1) && range(r->anchor, 0, 1) && cue_type_valid(r->cue_type) &&
           range(r->pad, 1, 8) && range(r->phrase_type, 0, 7) && range(r->phrase_index, 0, 10) &&
           range(r->phrase_point, 0, 2);
}
static int rule_position_valid(const cues_rule *r)
{
    return range(r->unit, 0, 2) && range(r->offset_unit, 0, 2) && num(r->position, 0, 86400) &&
           num(r->offset, -86400, 86400);
}
static int rule_loop_valid(const cues_rule *r)
{
    return range(r->loop_mode, 0, 3) && range(r->loop_unit, 0, 2) && num(r->loop_length, 0.1, 86400) &&
           (r->anchor != CUES_ABSOLUTE || r->loop_mode < CUES_LOOP_FROM_START);
}
static int rule_valid(const cues_rule *r)
{
    return rule_target_valid(r) && rule_position_valid(r) && rule_loop_valid(r) && range(r->color, 0, 8) &&
           terminated(r->name, sizeof r->name) && terminated(r->comment, sizeof r->comment);
}
int cues_config_validate(const cues_config *c, char *e, size_t n)
{
    if (!config_valid(c)) return errorf(e, n, "Invalid global configuration");
    for (int i = 0; i < c->group_count; i++) {
        const cues_group *g = &c->groups[i];
        if (!group_valid(g, c)) return errorf(e, n, "Invalid group or BPM range");
        for (int j = 0; j < g->rule_count; j++)
            if (!rule_valid(&g->rules[j])) return errorf(e, n, "Invalid rule");
    }
    if (n) e[0] = 0;
    return 1;
}
/* Bounded JSON token tree. next skips an entire subtree; children count is direct. */
typedef struct {
    int start, end, next, count;
    char type;
} tok;
typedef struct {
    const char *s;
    size_t len, p;
    tok *t;
    int used;
    char *error;
    size_t esize;
} parser;
#define TOKEN_LIMIT 24000
static void ws(parser *p)
{
    while (p->p < p->len && strchr(" \r\n\t", p->s[p->p]))
        p->p++;
}
static int fail(parser *p, const char *s)
{
    errorf(p->error, p->esize, s);
    return -1;
}
static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)s[i], n = c >= '0' && c <= '9'   ? c - '0'
                                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                : -1;
        if (n < 0)
            return -1;
        v = v * 16 + n;
    }
    return v;
}
static int utf8_sequence(unsigned lead, unsigned *value)
{
    if (lead >= 0xc2 && lead <= 0xdf) {
        *value = lead & 31;
        return 1;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
        *value = lead & 15;
        return 2;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
        *value = lead & 7;
        return 3;
    }
    return 0;
}
static int unicode_scalar(unsigned value, unsigned count)
{
    static const unsigned minimum[] = { 0, 0x80, 0x800, 0x10000 };
    return value >= minimum[count] && value <= 0x10ffff && (value < 0xd800 || value > 0xdfff);
}
static int utf8_tail(const unsigned char *s, unsigned count, unsigned value)
{
    for (unsigned j = 0; j < count; j++) {
        unsigned c = s[j];
        if ((c & 0xc0) != 0x80) return 0;
        value = (value << 6) | (c & 63);
    }
    return unicode_scalar(value, count);
}
static int utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], value;
        if (c < 0x80) {
            if (!c) return 0;
            continue;
        }
        unsigned count = (unsigned)utf8_sequence(c, &value);
        if (!count || i + count > n || !utf8_tail(s + i, count, value)) return 0;
        i += count;
    }
    return 1;
}
static int token(parser *p, int depth);
static int token_key(parser *p, int depth, tok *t)
{
    ws(p);
    if (p->p >= p->len || p->s[p->p] != '"') return fail(p, "Object key must be text");
    if (token(p, depth + 1) < 0) return -1;
    t->count++;
    ws(p);
    if (p->p >= p->len || p->s[p->p++] != ':') return fail(p, "Missing colon");
    return 1;
}
static int token_empty_container(parser *p, char close)
{
    ws(p);
    if (p->p >= p->len || p->s[p->p] != close) return 0;
    p->p++;
    return 1;
}

static int token_separator(parser *p, char close)
{
    if (p->p >= p->len) return fail(p, "Unclosed JSON container");
    char c = p->s[p->p++];
    if (c == close) return 1;
    return c == ',' ? 0 : fail(p, "Missing comma");
}
static int token_container(parser *p, int depth, tok *t)
{
    char close = t->type == '{' ? '}' : ']';
    if (token_empty_container(p, close)) return 1;
    for (;;) {
        if (t->type == '{' && token_key(p, depth, t) < 0) return -1;
        if (token(p, depth + 1) < 0) return -1;
        t->count++;
        ws(p);
        int end = token_separator(p, close);
        if (end) return end;
    }
}
static int token_escape(parser *p)
{
    if (p->p >= p->len) return fail(p, "Incomplete escape");
    unsigned char c = p->s[p->p++];
    if (c == 'u') {
        if (p->p + 4 > p->len || hex4(p->s + p->p) < 0) return fail(p, "Invalid Unicode escape");
        p->p += 4;
    } else if (!strchr("\"\\/bfnrt", c)) return fail(p, "Invalid escape");
    return 1;
}
static int token_text(parser *p)
{
    while (p->p < p->len) {
        unsigned char c = p->s[p->p++];
        if (c == '"') return 1;
        if (c < 32) return fail(p, "Control character in text");
        if (c == '\\' && token_escape(p) < 0) return -1;
    }
    return fail(p, "Unclosed text");
}
static int token_value(parser *p, int depth, tok *t)
{
    if (t->type == '{' || t->type == '[') return token_container(p, depth, t);
    if (t->type == '"') return token_text(p);
    while (p->p < p->len && !strchr(",]} \r\n\t", p->s[p->p])) p->p++;
    return 1;
}
static int token(parser *p, int depth)
{
    ws(p);
    if (depth > 16 || p->used >= TOKEN_LIMIT || p->p >= p->len) return fail(p, "JSON limit or incomplete value");
    int i = p->used++;
    tok *t = &p->t[i];
    t->start = (int)p->p;
    t->type = p->s[p->p++];
    if (token_value(p, depth, t) < 0) return -1;
    t->end = (int)p->p;
    t->next = p->used;
    return i;
}
static int escaped_surrogate_tail(parser *p, int a, int end)
{
    if (a + 6 >= end || p->s[a + 1] != '\\' || p->s[a + 2] != 'u')
        return fail(p, "Unpaired Unicode surrogate");
    int b = hex4(p->s + a + 3);
    if (b < 0xdc00 || b > 0xdfff) return fail(p, "Unpaired Unicode surrogate");
    return b;
}

static int escaped_unicode(parser *p, int *at, int end)
{
    int a = *at, u = hex4(p->s + a + 1);
    a += 4;
    if (u >= 0xd800 && u <= 0xdbff) {
        int b = escaped_surrogate_tail(p, a, end);
        if (b < 0) return -1;
        u = 0x10000 + (u - 0xd800) * 1024 + b - 0xdc00;
        a += 6;
    } else if (u >= 0xdc00 && u <= 0xdfff) return fail(p, "Unpaired Unicode surrogate");
    if (!u) return fail(p, "NUL in text");
    *at = a;
    return u;
}
static int encode_unicode(unsigned value, unsigned char buf[4])
{
    static const unsigned char prefix[] = { 0, 0, 0xc0, 0xe0, 0xf0 };
    int count = value < 128 ? 1 : value < 2048 ? 2
                              : value < 65536  ? 3
                                               : 4;
    for (int b = count - 1; b > 0; b--) {
        buf[b] = 0x80 | (value & 63);
        value >>= 6;
    }
    buf[0] = prefix[count] | value;
    return count;
}
static unsigned escape_character(unsigned c)
{
    static const char names[] = "bfnrt", values[] = "\b\f\n\r\t";
    const char *name = strchr(names, (int)c);
    return name ? (unsigned char)values[name - names] : c;
}
static int string_character(parser *p, int *at, int end, unsigned char bytes[4])
{
    unsigned c = (unsigned char)p->s[*at];
    if (c == '\\') {
        c = (unsigned char)p->s[++*at];
        if (c == 'u') {
            int value = escaped_unicode(p, at, end);
            return value < 0 ? -1 : encode_unicode((unsigned)value, bytes);
        }
        c = escape_character(c);
    }
    bytes[0] = (unsigned char)c;
    return 1;
}
static int str(parser *p, int i, char *out, size_t cap)
{
    tok *t = &p->t[i];
    if (t->type != '"') return fail(p, "Expected text");
    size_t n = 0;
    for (int a = t->start + 1; a < t->end - 1; a++) {
        unsigned char bytes[4];
        int count = string_character(p, &a, t->end, bytes);
        if (count < 0) return -1;
        if (n + (size_t)count >= cap) return fail(p, "Text too long");
        memcpy(out + n, bytes, (size_t)count);
        n += (size_t)count;
    }
    out[n] = 0;
    if (!utf8((const unsigned char *)out, n)) return fail(p, "Invalid UTF-8");
    return 1;
}
static int number_digits(parser *p, int *at, int end)
{
    int first = *at;
    while (*at < end && isdigit((unsigned char)p->s[*at])) ++*at;
    return *at > first;
}
static int number_whole(parser *p, int *at, int end)
{
    if (*at >= end) return fail(p, "Expected number");
    if (p->s[*at] == '-') ++*at;
    if (*at >= end) return fail(p, "Expected number");
    if (p->s[*at] == '0') {
        ++*at;
        return 1;
    }
    if (p->s[*at] < '1' || p->s[*at] > '9') return fail(p, "Expected number");
    number_digits(p, at, end);
    return 1;
}
static int number_fraction(parser *p, int *at, int end)
{
    if (*at < end && p->s[*at] == '.') {
        ++*at;
        if (!number_digits(p, at, end)) return fail(p, "Invalid number");
    }
    return 1;
}
static int number_exponent(parser *p, int *at, int end)
{
    if (*at < end && (p->s[*at] == 'e' || p->s[*at] == 'E')) {
        ++*at;
        if (*at < end && (p->s[*at] == '+' || p->s[*at] == '-')) ++*at;
        if (!number_digits(p, at, end)) return fail(p, "Invalid exponent");
    }
    return 1;
}
static int number(parser *p, int i, double *out)
{
    tok *t = &p->t[i];
    int at = t->start, end = t->end;
    if (number_whole(p, &at, end) < 0 || number_fraction(p, &at, end) < 0 || number_exponent(p, &at, end) < 0) return -1;
    if (at != end || end - t->start >= 64) return fail(p, "Invalid number");
    char buf[64];
    memcpy(buf, p->s + t->start, end - t->start);
    buf[end - t->start] = 0;
    *out = strtod(buf, NULL);
    if (!isfinite(*out)) return fail(p, "Non-finite number");
    return 1;
}
static int integer(parser *p, int i, int *out)
{
    double v;
    if (number(p, i, &v) < 0)
        return -1;
    if (v < -2147483647 || v > 2147483647 || floor(v) != v)
        return fail(p, "Expected integer");
    *out = (int)v;
    return 1;
}
static int boolean(parser *p, int i, int *out)
{
    tok *t = &p->t[i];
    int n = t->end - t->start;
    const char *s = p->s + t->start;
    if (n == 4 && !memcmp(s, "true", 4)) {
        *out = 1;
        return 1;
    }
    if (n == 5 && !memcmp(s, "false", 5)) {
        *out = 0;
        return 1;
    }
    return fail(p, "Expected boolean");
}
static int choice(parser *p, int i, const char *const *names, int count, int *out)
{
    char s[64];
    if (str(p, i, s, sizeof s) < 0)
        return -1;
    for (int j = 0; j < count; j++)
        if (!strcmp(s, names[j])) {
            *out = j;
            return 1;
        }
    return fail(p, "Unsupported choice");
}
static int obj(parser *p, int i)
{
    return p->t[i].type == '{' ? 1 : fail(p, "Expected object");
}
static int arr(parser *p, int i)
{
    return p->t[i].type == '[' ? 1 : fail(p, "Expected array");
}
/* Each parser checks all keys and rejects duplicates, so misspelled configuration cannot silently
 * run. */
static int field(parser *p, int key, const char *const *names, int count, unsigned long long *seen)
{
    char s[64];
    if (str(p, key, s, sizeof s) < 0)
        return -1;
    for (int i = 0; i < count; i++)
        if (!strcmp(s, names[i])) {
            if (*seen & (1ULL << i))
                return fail(p, "Duplicate field");
            *seen |= 1ULL << i;
            return i;
        }
    return fail(p, "Unsupported field (only Phrase and Absolute rules are supported)");
}
#define OBJECT(P, I, NAMES)                                                         \
    if (obj(P, I) < 0)                                                              \
        return -1;                                                                  \
    unsigned long long seen = 0;                                                    \
    for (int k = (I) + 1; k < (P)->t[I].next;) {                                    \
        int v = k + 1;                                                              \
        int f = field(P, k, NAMES, (int)(sizeof(NAMES) / sizeof(*(NAMES))), &seen); \
        if (f < 0)                                                                  \
            return -1;                                                              \
        k = (P)->t[v].next;
#define END_OBJECT }
#define CHECK(X)       \
    do {               \
        if ((X) < 0)   \
            return -1; \
    } while (0)
static int measure_unit(parser *p, int v, int *unit, int dynamic)
{
    char s[64];
    CHECK(str(p, v, s, sizeof s));
    int found = 0;
    for (int j = 0; j < 3; j++)
        if (!strcmp(s, units[j])) {
            *unit = j;
            found = 1;
        }
    if (dynamic && !strcmp(s, "from start of phrase")) {
        *unit = 3;
        found = 1;
    }
    if (dynamic && !strcmp(s, "to end of phrase")) {
        *unit = 4;
        found = 1;
    }
    if (!found)
        return fail(p, "Unsupported measure unit");
    return 1;
}

static int measure(parser *p, int i, double *value, int *unit, int dynamic)
{
    static const char *const keys[] = { "value", "unit" };
    OBJECT(p, i, keys)
    if (f == 0) {
        CHECK(number(p, v, value));
    } else {
        CHECK(measure_unit(p, v, unit, dynamic));
    }
    END_OBJECT if (!(seen & 2)) return fail(p, "Missing measure unit");
    return 1;
}
static int phrase_type(parser *p, int v, int *type)
{
    char s[64];
    CHECK(str(p, v, s, sizeof s));
    int found = 0;
    for (int j = 0; j < 8; j++)
        if (!strcmp(s, cues_phrase_names[j])) {
            *type = j;
            found = 1;
        }
    if (!strcmp(s, "Up")) {
        *type = 2;
        found = 1;
    }
    if (!strcmp(s, "Down") || !strcmp(s, "Breakdown")) {
        *type = 3;
        found = 1;
    }
    if (!found)
        return fail(p, "Unsupported phrase type");
    return 1;
}

static int phrase_index(parser *p, int v, int *index)
{
    if (p->t[v].type == '"') {
        char s[16];
        CHECK(str(p, v, s, sizeof s));
        if (!strcmp(s, "All"))
            *index = 0;
        else {
            char *end;
            long n = strtol(s, &end, 10);
            if (*end || n < 1 || n > 10)
                return fail(p, "Invalid phrase index");
            *index = (int)n;
        }
    } else
        CHECK(integer(p, v, &*index));
    return 1;
}

static int phrase(parser *p, int i, cues_rule *r)
{
    static const char *const keys[] = { "anchor", "type", "index" };
    OBJECT(p, i, keys)
    if (f == 0)
        CHECK(choice(p, v, anchors, 3, &r->phrase_point));
    else if (f == 1) {
        CHECK(phrase_type(p, v, &r->phrase_type));
    } else {
        CHECK(phrase_index(p, v, &r->phrase_index));
    }
    END_OBJECT return 1;
}
static int anchor(parser *p, int i, cues_rule *r)
{
    static const char *const keys[] = { "type", "phrases", "absolute" };
    OBJECT(p, i, keys)
    if (f == 0) {
        static const char *const a[] = { "phrase", "absolute" };
        CHECK(choice(p, v, a, 2, &r->anchor));
    } else if (f == 1) {
        CHECK(arr(p, v));
        if (p->t[v].count != 1)
            return fail(p, "Choose one phrase selector per rule");
        CHECK(phrase(p, v + 1, r));
    } else
        CHECK(measure(p, v, &r->position, &r->unit, 0));
    END_OBJECT if (!(seen & 1) ||
                   (r->anchor == CUES_PHRASE
                        ? !(seen & 2) || (seen & 4)
                        : !(seen & 4) || (seen & 2))) return fail(p, "Invalid anchor fields");
    return 1;
}
static int rule_pad(parser *p, int v, cues_rule *r, int *automatic_pad)
{
    char s[4];
    CHECK(str(p, v, s, sizeof s));
    if (!strcmp(s, "*")) {
        *automatic_pad = 1;
        r->pad = 1;
    } else if (s[0] < 'A' || s[0] > 'H' || s[1])
        return fail(p, "Invalid hot cue pad");
    else
        r->pad = s[0] - 'A' + 1;
    return 1;
}

static int rule_loop(parser *p, int v, cues_rule *r)
{
    int unit = r->loop_unit;
    CHECK(measure(p, v, &r->loop_length, &unit, 1));
    r->loop_mode = unit == 3   ? CUES_LOOP_FROM_START
                   : unit == 4 ? CUES_LOOP_TO_END
                               : CUES_LOOP_FIXED;
    if (unit < 3)
        r->loop_unit = unit;
    return 1;
}

static int rule_style(parser *p, int f, int v, cues_rule *r)
{
    if (f == 6)
        CHECK(choice(p, v, cues_color_names, 9, &r->color));
    else if (f == 7)
        CHECK(str(p, v, r->name, sizeof r->name));
    else
        CHECK(str(p, v, r->comment, sizeof r->comment));
    return 1;
}
static int rule_data_field(parser *p, int f, int v, cues_rule *r, int *automatic_pad)
{
    if (f == 0)
        CHECK(boolean(p, v, &r->enabled));
    else if (f == 1) {
        static const char *const types[] = { "hot", "memory" };
        int value;
        CHECK(choice(p, v, types, 2, &value));
        r->cue_type = value ? AZ_CUE_MEMORY : AZ_CUE_HOT;
    } else if (f == 2) {
        CHECK(rule_pad(p, v, r, automatic_pad));
    } else if (f == 3)
        CHECK(anchor(p, v, r));
    else if (f == 4)
        CHECK(measure(p, v, &r->offset, &r->offset_unit, 0));
    else if (f == 5) {
        CHECK(rule_loop(p, v, r));
    } else
        CHECK(rule_style(p, f, v, r));
    return 1;
}

static int rule_data(parser *p, int i, cues_rule *r)
{
    int automatic_pad = 0;
    static const char *const keys[] = { "enabled", "type", "pad", "anchor", "offset",
                                        "loop", "color", "name", "comment" };
    OBJECT(p, i, keys)
    CHECK(rule_data_field(p, f, v, r, &automatic_pad));
    END_OBJECT if ((seen & 10) != 10) return fail(p, "Rule needs type and anchor");
    if (automatic_pad && !cues_rule_auto_pad(r))
        return fail(p, "Automatic hot pads require All phrase occurrences");
    return 1;
}
static int rule(parser *p, int i, cues_rule *r)
{
    static const char *const keys[] = { "data", "sentence" };
    OBJECT(p, i, keys)
    if (f == 0)
        CHECK(rule_data(p, v, r));
    else {
        char ignored[1024];
        CHECK(str(p, v, ignored, sizeof ignored));
    }
    END_OBJECT if (!(seen & 1)) return fail(p, "Rule needs data");
    return 1;
}
static int tags(parser *p, int i, cues_group *g)
{
    static const char *const keys[] = { "artist", "key" };
    OBJECT(p, i, keys)
    if (f == 0)
        CHECK(str(p, v, g->artist, sizeof g->artist));
    else
        CHECK(str(p, v, g->key, sizeof g->key));
    END_OBJECT return 1;
}
static int bpm(parser *p, int i, cues_group *g)
{
    static const char *const keys[] = { "min", "max" };
    OBJECT(p, i, keys)
    if (p->t[v].end - p->t[v].start == 4 && !memcmp(p->s + p->t[v].start, "null", 4)) {
        if (f == 0)
            g->min_bpm = 0;
        else
            g->max_bpm = 0;
    } else
        CHECK(number(p, v, f == 0 ? &g->min_bpm : &g->max_bpm));
    END_OBJECT return 1;
}
static int group_rules(parser *p, int rules_token, cues_group *g)
{
    if (rules_token >= 0) {
        int v = rules_token;
        CHECK(arr(p, v));
        if (p->t[v].count > CUES_MAX_RULES)
            return fail(p, "Too many rules (maximum 16 per group)");
        g->rule_count = p->t[v].count;
        int a = v + 1;
        for (int j = 0; j < g->rule_count; j++) {
            cues_rule_default(&g->rules[j], g);
            CHECK(rule(p, a, &g->rules[j]));
            a = p->t[a].next;
        }
    }
    return 1;
}

static int group(parser *p, int i, cues_group *g)
{
    int rules_token = -1;
    static const char *const keys[] = { "enabled", "name", "comment", "tags",
                                        "quantization", "bpmRange", "measuresIn", "rules" };
    OBJECT(p, i, keys)
    if (f == 0)
        CHECK(boolean(p, v, &g->enabled));
    else if (f == 1)
        CHECK(str(p, v, g->name, sizeof g->name));
    else if (f == 2)
        CHECK(str(p, v, g->comment, sizeof g->comment));
    else if (f == 3)
        CHECK(tags(p, v, g));
    else if (f == 4)
        CHECK(choice(p, v, quant, 4, &g->quantization));
    else if (f == 5)
        CHECK(bpm(p, v, g));
    else if (f == 6)
        CHECK(choice(p, v, measures, 3, &g->measures_in));
    else
        rules_token = v;
    END_OBJECT
    return group_rules(p, rules_token, g);
}
static int global_field(parser *p, int f, int v, cues_config *c)
{
    if (f == 0) return boolean(p, v, &c->enabled);
    if (f == 1) return integer(p, v, &c->max_memory);
    if (f == 2) return integer(p, v, &c->hot_pads);
    if (f >= 3 && f <= 5) {
        double *values[] = { &c->min_bpm, &c->max_bpm, &c->min_distance };
        return number(p, v, values[f - 3]);
    }
    if (f == 6) {
        static const char *const pref[] = { "Hot Cue", "Memory Cue" };
        int a;
        CHECK(choice(p, v, pref, 2, &a));
        c->cue_preference = a ? AZ_CUE_MEMORY : AZ_CUE_HOT;
        return 1;
    }
    if (f == 7) return choice(p, v, quant, 4, &c->quantization);
    return choice(p, v, measures, 3, &c->measures_in);
}

static int globals(parser *p, int i, cues_config *c)
{
    static const char *const keys[] = { "enabled", "maxMemoryCues", "hotCuePads",
                                        "minBpm", "maxBpm", "minDistance",
                                        "cuePreference", "quantization", "measuresIn" };
    OBJECT(p, i, keys)
    CHECK(global_field(p, f, v, c));
    END_OBJECT return 1;
}
static int root(parser *p, cues_config *c)
{
    int groups_token = -1;
    static const char *const keys[] = { "version", "config", "groups" };
    OBJECT(p, 0, keys)
    if (f == 0) {
        int vnum;
        CHECK(integer(p, v, &vnum));
        if (vnum != 1)
            return fail(p, "Unsupported config version");
    } else if (f == 1)
        CHECK(globals(p, v, c));
    else
        groups_token = v;
    END_OBJECT if (!(seen & 2) || !(seen & 4)) return fail(p,
                                                           "Configuration needs config and groups");
    int v = groups_token;
    CHECK(arr(p, v));
    if (p->t[v].count > CUES_MAX_GROUPS)
        return fail(p, "Too many groups (maximum 8)");
    c->group_count = p->t[v].count;
    int a = v + 1;
    for (int j = 0; j < c->group_count; j++) {
        cues_group_default(&c->groups[j], c);
        CHECK(group(p, a, &c->groups[j]));
        a = p->t[a].next;
    }
    return 1;
}
static int parse_config_tree(parser *p, cues_config *next)
{
    int okay = token(p, 0) >= 0;
    ws(p);
    if (okay && p->p != p->len) {
        fail(p, "Trailing JSON content");
        okay = 0;
    }
    if (okay)
        okay = root(p, next) > 0;
    if (okay)
        okay = cues_config_validate(next, p->error, p->esize);
    return okay;
}

int cues_config_parse(cues_config *out, const char *json, size_t length, char *error, size_t n)
{
    if (!out || !json || !length || length > 128 * 1024)
        return errorf(error, n, "JSON must be 1 byte to 128 KiB");
    tok *tokens = calloc(TOKEN_LIMIT, sizeof *tokens);
    cues_config *next = malloc(sizeof *next);
    if (!tokens || !next) {
        free(tokens);
        free(next);
        return errorf(error, n, "Out of memory");
    }
    cues_config_default(next);
    parser p = { json, length, 0, tokens, 0, error, n };
    if (n)
        error[0] = 0;
    int okay = parse_config_tree(&p, next);
    if (okay)
        *out = *next;
    free(tokens);
    free(next);
    return okay;
}
/* Source-compatible JSON serialization, excluding analysis-only options. */
typedef struct {
    char *s;
    size_t n, p;
    int okay;
} writer;
static void writef(writer *w, const char *fmt, ...)
{
    if (!w->okay)
        return;
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(w->s + w->p, w->n - w->p, fmt, a);
    va_end(a);
    if (n < 0 || (size_t)n >= w->n - w->p) {
        w->okay = 0;
        return;
    }
    w->p += (size_t)n;
}
static void quoted(writer *w, const char *s)
{
    writef(w, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            writef(w, "\\%c", *p);
        else if (*p < 32)
            writef(w, "\\u%04x", *p);
        else
            writef(w, "%c", *p);
    }
    writef(w, "\"");
}
static void write_anchor(writer *w, const cues_rule *r)
{
    writef(w, ",\"anchor\":{\"type\":\"%s\",",
           r->anchor == CUES_PHRASE ? "phrase" : "absolute");
    if (r->anchor == CUES_PHRASE) {
        writef(w, "\"phrases\":[{\"anchor\":\"%s\",\"type\":\"%s\",\"index\":\"",
               anchors[r->phrase_point], cues_phrase_names[r->phrase_type]);
        if (r->phrase_index)
            writef(w, "%d", r->phrase_index);
        else
            writef(w, "All");
        writef(w, "\"}]}");
    } else
        writef(w, "\"absolute\":{\"value\":%.17g,\"unit\":\"%s\"}}", r->position,
               units[r->unit]);
    if (r->anchor == CUES_PHRASE)
        writef(w, ",\"offset\":{\"value\":%.17g,\"unit\":\"%s\"}", r->offset,
               units[r->offset_unit]);
}
static void write_rule(writer *w, const cues_rule *r, int j)
{
    writef(w, "%s\n      {\"data\":{\"enabled\":%s,\"type\":\"%s\"", j ? "," : "",
           r->enabled ? "true" : "false", r->cue_type == AZ_CUE_HOT ? "hot" : "memory");
    if (r->cue_type == AZ_CUE_HOT)
        writef(w, ",\"pad\":\"%c\"", cues_rule_auto_pad(r) ? '*' : 'A' + r->pad - 1);
    write_anchor(w, r);
    if (r->loop_mode)
        writef(w, ",\"loop\":{\"value\":%.17g,\"unit\":\"%s\"}", r->loop_length,
               r->loop_mode == CUES_LOOP_FROM_START ? "from start of phrase"
               : r->loop_mode == CUES_LOOP_TO_END   ? "to end of phrase"
                                                    : units[r->loop_unit]);
    writef(w, ",\"color\":\"%s\",\"name\":", cues_color_names[r->color]);
    quoted(w, r->name);
    writef(w, ",\"comment\":");
    quoted(w, r->comment);
    writef(w, "}}");
}
static void write_group(writer *w, const cues_group *g, int i)
{
    writef(w, "%s\n    {\"enabled\":%s,\"name\":", i ? "," : "",
           g->enabled ? "true" : "false");
    quoted(w, g->name);
    writef(w, ",\"comment\":");
    quoted(w, g->comment);
    writef(w, ",\"tags\":{\"artist\":");
    quoted(w, g->artist);
    writef(w, ",\"key\":");
    quoted(w, g->key);
    writef(w,
           "},\"quantization\":\"%s\",\"bpmRange\":{\"min\":%.17g,\"max\":%.17g},"
           "\"measuresIn\":\"%s\",\"rules\":[",
           quant[g->quantization], g->min_bpm, g->max_bpm, measures[g->measures_in]);
    for (int j = 0; j < g->rule_count; j++) write_rule(w, &g->rules[j], j);
    writef(w, "\n    ]}");
}

int cues_config_json(const cues_config *c, char *json, size_t capacity)
{
    char error[128];
    if (!json || !capacity || !cues_config_validate(c, error, sizeof error))
        return 0;
    writer w = { json, capacity, 0, 1 };
    writef(&w,
           "{\n  \"version\":1,\n  "
           "\"config\":{\"enabled\":%s,\"maxMemoryCues\":%d,\"hotCuePads\":%d,\"minBpm\":%.17g,"
           "\"maxBpm\":%.17g,\"minDistance\":%.17g,\"cuePreference\":\"%s\",\"quantization\":\"%"
           "s\",\"measuresIn\":\"%s\"},\n  \"groups\":[",
           c->enabled ? "true" : "false", c->max_memory, c->hot_pads, c->min_bpm, c->max_bpm,
           c->min_distance, c->cue_preference == AZ_CUE_HOT ? "Hot Cue" : "Memory Cue",
           quant[c->quantization], measures[c->measures_in]);
    for (int i = 0; i < c->group_count; i++) write_group(&w, &c->groups[i], i);
    writef(&w, "\n  ]\n}\n");
    return w.okay ? (int)w.p : 0;
}
