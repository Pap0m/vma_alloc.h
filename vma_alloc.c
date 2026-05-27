#include <stddef.h>

#include "vma_alloc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <threads.h>

// 2MB Chunk definitions
#define CHUNK_SIZE_SHIFT 21
#define CHUNK_SIZE (1ULL << CHUNK_SIZE_SHIFT) // 2,097,152 bytes
#define CHUNK_MASK (~(CHUNK_SIZE - 1))        // Used to find the header

// Slab definitions
#define MAX_SLAB_SIZE 4096
#define SLAB_CLASS_COUNT 11

// Helper macro: Given any pointer, mask it to find its ChunkHeader
#define GET_CHUNK_HEADER(ptr) ((ChunkHeader *)((uintptr_t)(ptr) & CHUNK_MASK))

typedef struct {
  struct Block_Node *next;
} Block_Node;

typedef struct {
  pthread_t thread_owner_id;
  uint32_t block_size;

  uint32_t active_allocations;

  // memory tracking
  char *dump_ptr;
  size_t free_space;

  // linked list of chunks
  struct Chunk_Header *next_chunk;
  struct Chunk_Header *prev_chunk;
} Chunk_Header;

typedef struct {
  uint32_t block_size;

  // linked list of freed blocks ready to be reused
  Block_Node *free_list;

  // double linked list of chunks currently actively serving this slab size
  Chunk_Header *active_chunks;
} Slab_Class;

typedef struct {
  pthread_t thread_id;

  // array of slab classes (index 1=8b, 2=16b... 11=4096, 0=large)
  Slab_Class slabs[SLAB_CLASS_COUNT];

  // lock-free mailbox for remote frees
  // must be _Atomic so other threads can safely push to it
  _Atomic(Block_Node *) mailbox;
} Thread_Arena;

typedef struct {
  _Atomic(Chunk_Header *) cached_empty_chunks;
} Global_Heap;

void *vma_alloc(size_t size) {}

void vma_free(void *ptr) {}
