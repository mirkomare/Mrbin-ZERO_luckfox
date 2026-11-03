#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    cJSON_Invalid = 0,
    cJSON_False = 1,
    cJSON_True = 2,
    cJSON_NULL = 4,
    cJSON_Number = 8,
    cJSON_String = 16,
    cJSON_Array = 32,
    cJSON_Object = 64,
} cJSON_Type;

typedef struct cJSON {
    struct cJSON *next, *prev, *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

cJSON *cJSON_Parse(const char *s);
void cJSON_Delete(cJSON *c);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *o, const char *k);
int cJSON_IsString(const cJSON *c);
int cJSON_IsNumber(const cJSON *c);
int cJSON_IsArray(const cJSON *c);

#define cJSON_ArrayForEach(el, arr) \
    for (cJSON *el = ((arr) ? (arr)->child : nullptr); el; el = el->next)

#ifdef __cplusplus
}
#endif
