#include <stddef.h>

#include "vma_alloc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

// 2MB Chunk definitions
#define CHUNK_SIZE_SHIFT 21
#define CHUNK_SIZE (1ULL << CHUNK_SIZE_SHIFT) // 2 MB
#define CHUNK_MASK (~(CHUNK_SIZE - 1))        // Used to find the header

// Slab definitions
#define MAX_SLAB_SIZE 4096
#define SLAB_CLASS_COUNT 11

// Helper macro: Given any pointer, mask it to find its ChunkHeader
#define GET_CHUNK_HEADER(ptr) ((Chunk_Header *)((uintptr_t)(ptr) & CHUNK_MASK))

typedef struct Block_Node {
  struct Block_Node *next;
} Block_Node;

typedef struct Chunk_Header {
  pthread_t thread_owner_id;
  struct Thread_Arena *arena_owner;
  uint32_t block_size;

  uint32_t active_allocations;

  // memory tracking
  char *dump_ptr;
  size_t free_space;

  // linked list of chunks
  struct Chunk_Header *next_chunk;
  struct Chunk_Header *prev_chunk;
} Chunk_Header;

typedef struct Slab_Class {
  uint32_t block_size;

  // linked list of freed blocks ready to be reused
  Block_Node *free_list;

  // double linked list of chunks currently actively serving this slab size
  Chunk_Header *active_chunks;
} Slab_Class;

typedef struct Thread_Arena {
  pthread_t thread_id;

  // array of slab classes (index 1=8b, 2=16b... 11=4096, 0=large)
  Slab_Class slabs[SLAB_CLASS_COUNT];

  // lock-free mailbox for remote frees
  // must be _Atomic so other threads can safely push to it
  _Atomic(Block_Node *) mailbox;
} Thread_Arena;

typedef struct Global_Heap {
  _Atomic(Chunk_Header *) cached_empty_chunks;
} Global_Heap;

static _Thread_local Thread_Arena local_arena = {0};
static _Thread_local bool local_arena_initialized = false;
static Global_Heap shared_memory = {0};

static const uint32_t slab_sizes[SLAB_CLASS_COUNT] = {
    0, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

uint32_t get_slab_index(size_t size) {
  for (uint32_t i = 0; i < SLAB_CLASS_COUNT; ++i) {
    if (size <= slab_sizes[i]) {
      return slab_sizes[i];
    }
  }
  return 0;
}

void *mmap_aligned_2mb() {
  size_t alloc_size = CHUNK_SIZE * 2;
  void *ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED)
    return MAP_FAILED;

  uintptr_t addr = (uintptr_t)ptr;
  uintptr_t aligned_addr = (addr + CHUNK_SIZE - 1) & CHUNK_MASK;

  // Trim unaligned prefix and suffix spaces to stay resource-friendly
  size_t prefix_size = aligned_addr - addr;
  if (prefix_size > 0) {
    munmap(ptr, prefix_size);
  }
  size_t suffix_size = CHUNK_SIZE - prefix_size;
  if (suffix_size > 0) {
    munmap((void *)(aligned_addr + CHUNK_SIZE), suffix_size);
  }

  return (void *)aligned_addr;
}

void process_mailbox(Thread_Arena *arena) {
  Block_Node *head = atomic_exchange(&arena->mailbox, NULL);
  while (head) {
    Block_Node *next = head->next;
    Chunk_Header *chunk = GET_CHUNK_HEADER(head);

    uint32_t b_size = chunk->block_size;
    uint32_t index = get_slab_index(b_size);

    if (!index) {
      head->next = arena->slabs[index].free_list;
      arena->slabs[index].free_list = head;
    }

    chunk->active_allocations--;
    head = next;
  }
}

void init_local_arena() {
  local_arena.thread_id = pthread_self();
  for (int i = 0; i < SLAB_CLASS_COUNT; ++i) {
    local_arena.slabs[i].block_size = slab_sizes[i];
    local_arena.slabs[i].free_list = NULL;
    local_arena.slabs[i].active_chunks = NULL;
  }
  atomic_init(&local_arena.mailbox, 0);
  local_arena_initialized = true;
}

void *vma_alloc(size_t size) {
  if (size <= 0)
    return NULL;

  uint32_t index = get_slab_index(size);
  if (!index) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
  }

  if (!local_arena_initialized) {
    init_local_arena();
  }

  // clear cross-thread frees
  process_mailbox(&local_arena);

  Slab_Class *slab = &local_arena.slabs[index];

  // pop from the mailbox
  if (slab->free_list) {
    Block_Node *node = slab->free_list;
    slab->free_list = node->next;
    Chunk_Header *chunk = GET_CHUNK_HEADER(node);
    chunk->active_allocations++;
    return (void *)node;
  }

  // bump from the chunk
  Chunk_Header *chunk = slab->active_chunks;
  if (!chunk || chunk->free_space < index) {
    chunk = mmap_aligned_2mb();
    if (chunk == MAP_FAILED)
      return NULL;

    chunk->thread_owner_id = pthread_self();
    chunk->arena_owner = &local_arena;
    chunk->block_size = index;
    chunk->active_allocations = 0;

    chunk->dump_ptr = (char *)chunk + sizeof(Chunk_Header);
    uintptr_t align_pad = (uintptr_t)chunk->dump_ptr % 8;
    if (align_pad != 0)
      chunk->dump_ptr += (8 - align_pad);

    chunk->free_space = CHUNK_SIZE - (chunk->dump_ptr - (char *)chunk);

    // push into current slab double linked list
    chunk->next_chunk = slab->active_chunks;
    if (slab->active_chunks) {
      slab->active_chunks->prev_chunk = chunk;
    }
    chunk->prev_chunk = NULL;
    slab->active_chunks = chunk;
  }

  void *allocated_ptr = (void *)chunk->dump_ptr;
  chunk->dump_ptr += index;
  chunk->free_space -= index;
  chunk->active_allocations++;

  return allocated_ptr;
}

void vma_free(void *ptr) {}
