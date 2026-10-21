#pragma once
#include <stdlib.h>
static inline void *helix_malloc(int size) { return malloc((size_t)size); }
static inline void helix_free(void *ptr) { free(ptr); }
