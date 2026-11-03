#pragma once

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_DEFAULT  0
#define MALLOC_CAP_8BIT     1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_SPIRAM   4
#define MALLOC_CAP_DMA      8

void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_realloc(void *p, size_t size, unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);

#ifdef __cplusplus
}
#endif
