#pragma once

#ifndef VMA_ALLOC_H
#define VMA_ALLOC_H

#include <stddef.h>

// API
void *vma_alloc(size_t size);
void vma_free(void *ptr);

#ifdef VMA_ALLOC_IMPLEMENTATION

#endif // VMA_ALLOC_IMPLEMENTATION

#endif // VMA_ALLOC_H
