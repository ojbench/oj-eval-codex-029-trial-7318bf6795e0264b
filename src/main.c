#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGNMENT ((size_t)alignof(max_align_t))
#define DEFAULT_CHUNK_SIZE ((size_t)1 << 20)
#define MIN_SPLIT_REMAINDER (sizeof(BlockHeader) + ALIGNMENT)

typedef struct BlockHeader BlockHeader;

struct BlockHeader {
    size_t size;
    bool is_free;
    BlockHeader *prev_phys;
    BlockHeader *next_phys;
    BlockHeader *prev_free;
    BlockHeader *next_free;
};

static BlockHeader *heap_head = NULL;
static BlockHeader *heap_tail = NULL;
static BlockHeader *free_head = NULL;
static atomic_flag heap_lock = ATOMIC_FLAG_INIT;

static size_t align_up(size_t value) {
    size_t mask = ALIGNMENT - 1;
    return (value + mask) & ~mask;
}

static void lock_heap(void) {
    while (atomic_flag_test_and_set_explicit(&heap_lock, memory_order_acquire)) {
    }
}

static void unlock_heap(void) {
    atomic_flag_clear_explicit(&heap_lock, memory_order_release);
}

static void free_list_remove(BlockHeader *block) {
    if (block->prev_free != NULL) {
        block->prev_free->next_free = block->next_free;
    } else if (free_head == block) {
        free_head = block->next_free;
    }

    if (block->next_free != NULL) {
        block->next_free->prev_free = block->prev_free;
    }

    block->prev_free = NULL;
    block->next_free = NULL;
}

static void free_list_insert(BlockHeader *block) {
    block->is_free = true;
    block->prev_free = NULL;
    block->next_free = free_head;
    if (free_head != NULL) {
        free_head->prev_free = block;
    }
    free_head = block;
}

static BlockHeader *request_chunk(size_t min_payload) {
    size_t payload = min_payload;
    size_t chunk_payload = payload > DEFAULT_CHUNK_SIZE ? payload : DEFAULT_CHUNK_SIZE;
    size_t total_size = align_up(sizeof(BlockHeader) + chunk_payload);

    void *memory = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return NULL;
    }

    BlockHeader *block = (BlockHeader *)memory;
    block->size = total_size - sizeof(BlockHeader);
    block->is_free = true;
    block->prev_phys = heap_tail;
    block->next_phys = NULL;
    block->prev_free = NULL;
    block->next_free = NULL;

    if (heap_tail != NULL) {
        heap_tail->next_phys = block;
    } else {
        heap_head = block;
    }
    heap_tail = block;

    free_list_insert(block);
    return block;
}

static void split_block(BlockHeader *block, size_t requested_size) {
    size_t aligned_size = align_up(requested_size);
    if (block->size < aligned_size + MIN_SPLIT_REMAINDER) {
        return;
    }

    unsigned char *base = (unsigned char *)(block + 1);
    BlockHeader *remainder = (BlockHeader *)(base + aligned_size);
    remainder->size = block->size - aligned_size - sizeof(BlockHeader);
    remainder->is_free = true;
    remainder->prev_phys = block;
    remainder->next_phys = block->next_phys;
    remainder->prev_free = NULL;
    remainder->next_free = NULL;

    if (remainder->next_phys != NULL) {
        remainder->next_phys->prev_phys = remainder;
    } else {
        heap_tail = remainder;
    }

    block->size = aligned_size;
    block->next_phys = remainder;
    free_list_insert(remainder);
}

static BlockHeader *coalesce(BlockHeader *block) {
    if (block->prev_phys != NULL && block->prev_phys->is_free) {
        BlockHeader *prev = block->prev_phys;
        free_list_remove(prev);
        prev->size += sizeof(BlockHeader) + block->size;
        prev->next_phys = block->next_phys;
        if (block->next_phys != NULL) {
            block->next_phys->prev_phys = prev;
        } else {
            heap_tail = prev;
        }
        block = prev;
    }

    if (block->next_phys != NULL && block->next_phys->is_free) {
        BlockHeader *next = block->next_phys;
        free_list_remove(next);
        block->size += sizeof(BlockHeader) + next->size;
        block->next_phys = next->next_phys;
        if (block->next_phys != NULL) {
            block->next_phys->prev_phys = block;
        } else {
            heap_tail = block;
        }
    }

    return block;
}

static BlockHeader *find_fit(size_t size) {
    for (BlockHeader *block = free_head; block != NULL; block = block->next_free) {
        if (block->size >= size) {
            return block;
        }
    }
    return NULL;
}

void *malloc(size_t size) {
    if (size == 0) {
        size = 1;
    }

    size_t aligned_size = align_up(size);
    lock_heap();

    BlockHeader *block = find_fit(aligned_size);
    if (block == NULL) {
        if (request_chunk(aligned_size) == NULL) {
            unlock_heap();
            return NULL;
        }
        block = find_fit(aligned_size);
    }

    free_list_remove(block);
    split_block(block, aligned_size);
    block->is_free = false;

    unlock_heap();
    return (void *)(block + 1);
}

void free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    BlockHeader *block = ((BlockHeader *)ptr) - 1;
    lock_heap();
    if (block->is_free) {
        unlock_heap();
        return;
    }

    block->is_free = true;
    block = coalesce(block);
    free_list_insert(block);
    unlock_heap();
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }

    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    size_t aligned_size = align_up(size);
    BlockHeader *block = ((BlockHeader *)ptr) - 1;

    lock_heap();
    if (block->size >= aligned_size) {
        split_block(block, aligned_size);
        unlock_heap();
        return ptr;
    }

    if (block->next_phys != NULL && block->next_phys->is_free &&
        block->size + sizeof(BlockHeader) + block->next_phys->size >= aligned_size) {
        BlockHeader *next = block->next_phys;
        free_list_remove(next);
        block->size += sizeof(BlockHeader) + next->size;
        block->next_phys = next->next_phys;
        if (block->next_phys != NULL) {
            block->next_phys->prev_phys = block;
        } else {
            heap_tail = block;
        }
        split_block(block, aligned_size);
        unlock_heap();
        return ptr;
    }
    unlock_heap();

    void *new_ptr = malloc(size);
    if (new_ptr == NULL) {
        return NULL;
    }

    size_t copy_size = block->size < size ? block->size : size;
    memcpy(new_ptr, ptr, copy_size);
    free(ptr);
    return new_ptr;
}

static int run_allocator_checks(void) {
    char *a = (char *)malloc(32);
    if (a == NULL) {
        return 1;
    }
    for (int i = 0; i < 32; ++i) {
        a[i] = (char)(i + 1);
    }

    char *b = (char *)malloc(64);
    if (b == NULL) {
        free(a);
        return 1;
    }
    memset(b, 7, 64);

    free(a);
    char *c = (char *)malloc(16);
    if (c == NULL) {
        free(b);
        return 1;
    }

    int zero_ok = 1;
    unsigned char *d = (unsigned char *)calloc(128, sizeof(unsigned char));
    if (d == NULL) {
        free(b);
        free(c);
        return 1;
    }
    for (int i = 0; i < 128; ++i) {
        if (d[i] != 0) {
            zero_ok = 0;
            break;
        }
    }

    char *grown = (char *)realloc(b, 256);
    if (grown == NULL) {
        free(c);
        free(d);
        return 1;
    }
    for (int i = 0; i < 64; ++i) {
        if ((unsigned char)grown[i] != 7U) {
            zero_ok = 0;
            break;
        }
    }

    free(c);
    free(d);
    free(grown);

    void *big = malloc((size_t)3 << 20);
    if (big == NULL) {
        return 1;
    }
    free(big);

    return zero_ok ? 0 : 1;
}

int main(void) {
    return run_allocator_checks();
}
