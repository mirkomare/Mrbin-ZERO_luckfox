#include "hal_cjson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip(const char *s) {
    while (s && *s && isspace((unsigned char)*s)) ++s;
    return s;
}

static char *dupn(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return nullptr;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static cJSON *new_item(int type) {
    cJSON *c = (cJSON *)calloc(1, sizeof(*c));
    if (c) c->type = type;
    return c;
}

static const char *parse_value(cJSON *item, const char *s);

static const char *parse_string(cJSON *item, const char *s) {
    if (*s != '"') return nullptr;
    ++s;
    const char *start = s;
    while (*s && *s != '"') {
        if (*s == '\\' && s[1]) s += 2;
        else ++s;
    }
    item->type = cJSON_String;
    item->valuestring = dupn(start, (size_t)(s - start));
    return *s == '"' ? s + 1 : nullptr;
}

static const char *parse_number(cJSON *item, const char *s) {
    char *end = nullptr;
    double v = strtod(s, &end);
    item->type = cJSON_Number;
    item->valuedouble = v;
    item->valueint = (int)v;
    return end;
}

static void add_child(cJSON *obj, cJSON *ch) {
    if (!obj->child) {
        obj->child = ch;
        return;
    }
    cJSON *p = obj->child;
    while (p->next) p = p->next;
    p->next = ch;
    ch->prev = p;
}

static const char *parse_object(cJSON *item, const char *s) {
    item->type = cJSON_Object;
    s = skip(s + 1);
    if (*s == '}') return s + 1;
    while (*s) {
        cJSON *ch = new_item(0);
        s = skip(s);
        if (*s != '"') {
            cJSON_Delete(ch);
            return nullptr;
        }
        cJSON tmp = {};
        s = parse_string(&tmp, s);
        ch->string = tmp.valuestring;
        s = skip(s);
        if (!s || *s != ':') {
            cJSON_Delete(ch);
            return nullptr;
        }
        s = parse_value(ch, skip(s + 1));
        if (!s) {
            cJSON_Delete(ch);
            return nullptr;
        }
        add_child(item, ch);
        s = skip(s);
        if (*s == ',') {
            s = skip(s + 1);
            continue;
        }
        if (*s == '}') return s + 1;
        return nullptr;
    }
    return nullptr;
}

static const char *parse_array(cJSON *item, const char *s) {
    item->type = cJSON_Array;
    s = skip(s + 1);
    if (*s == ']') return s + 1;
    while (*s) {
        cJSON *ch = new_item(0);
        s = parse_value(ch, skip(s));
        if (!s) {
            cJSON_Delete(ch);
            return nullptr;
        }
        add_child(item, ch);
        s = skip(s);
        if (*s == ',') {
            s = skip(s + 1);
            continue;
        }
        if (*s == ']') return s + 1;
        return nullptr;
    }
    return nullptr;
}

static const char *parse_value(cJSON *item, const char *s) {
    s = skip(s);
    if (!s) return nullptr;
    if (*s == '"') return parse_string(item, s);
    if (*s == '{') return parse_object(item, s);
    if (*s == '[') return parse_array(item, s);
    if (*s == '-' || isdigit((unsigned char)*s)) return parse_number(item, s);
    if (strncmp(s, "true", 4) == 0) {
        item->type = cJSON_True;
        item->valueint = 1;
        return s + 4;
    }
    if (strncmp(s, "false", 5) == 0) {
        item->type = cJSON_False;
        return s + 5;
    }
    if (strncmp(s, "null", 4) == 0) {
        item->type = cJSON_NULL;
        return s + 4;
    }
    return nullptr;
}

cJSON *cJSON_Parse(const char *s) {
    if (!s) return nullptr;
    cJSON *root = new_item(0);
    if (!parse_value(root, s)) {
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

void cJSON_Delete(cJSON *c) {
    while (c) {
        cJSON *n = c->next;
        if (c->child) cJSON_Delete(c->child);
        free(c->valuestring);
        free(c->string);
        free(c);
        c = n;
    }
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *o, const char *k) {
    if (!o || !k) return nullptr;
    for (cJSON *c = o->child; c; c = c->next) {
        if (c->string && strcmp(c->string, k) == 0) return c;
    }
    return nullptr;
}

int cJSON_IsString(const cJSON *c) { return c && c->type == cJSON_String && c->valuestring; }
int cJSON_IsNumber(const cJSON *c) { return c && c->type == cJSON_Number; }
int cJSON_IsArray(const cJSON *c) { return c && c->type == cJSON_Array; }
