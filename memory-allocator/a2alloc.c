#define _GNU_SOURCE

#include <stdlib.h>
#include <pthread.h>
#include <strings.h>
#include <sched.h>
#include <assert.h>

#include "memlib.h"
#include "mm_thread.h"

////////////////////////////////////////
// Parameters:

// Specific values used in the Hoard paper are 4, 4, and 8192.
#define K_THRESH 4 // K-value
#define NBINS 4 // f-value
#define SB_SIZE 1024 // S-value

#define NSIZES 7
const size_t sizes[NSIZES] = {
    8, 16, 32, 64, 128, 256, 450
};

////////////////////////////////////////
// Macros:

typedef unsigned long vaddr_t;

#define CACHE_LINE_SZ 64

#define ALIGN(p, N)    (((vaddr_t)(p) / N) * N)
#define ALIGN_UP(p, N) ((((vaddr_t)(p) + N - 1) / N) * N)

#define SB_ALIGN(p)    ALIGN(p, SB_SIZE)
#define SB_ALIGN_UP(p) ALIGN_UP(p, SB_SIZE)

#define SBRK(inc) mem_sbrk(SB_ALIGN_UP(inc))

////////////////////////////////////////
// Structures:

// A subheap is a collection of superblocks within a given parent heap,
// corresponding to a particular size class, divided into bins based on
// fullness.
struct subheap {
    struct superblock *bins[NBINS];
    struct superblock *full_bin;
    int allocated; // Number of allocated blocks.
    int used;      // Number of used blocks.
    pthread_mutex_t lock;
};

struct heap {
    struct subheap subheaps[NSIZES];
    struct superblock *empty_bin;
    int num_empties;
    pthread_mutex_t empties_lock;
} __attribute__ ((aligned (CACHE_LINE_SZ))); // Pad to cache-line size.

#define EMPTY_SC -1
#define LARGE_SC -2

struct superblock {
    signed char sc;
    int used; // Number of used blocks.
    struct superblock **bin; // Containing bin.
    struct superblock *prev;
    struct superblock *next;
    struct freelist *freelist;
};

struct freelist {
    struct freelist *next;
};

struct largeblock {
    signed char sc;
    int num_superblocks;
};

////////////////////////////////////////
// Globals:

pthread_mutex_t sbrk_lock = PTHREAD_MUTEX_INITIALIZER;

int num_cpus;

int max_blocks[NSIZES];

struct heap *global_heap;
struct heap *cpu_heaps;

////////////////////////////////////////
// Prototypes:

void check_superblock(struct superblock *sb, int sc, struct superblock **bin);
void init_subheap(struct subheap *subheap);
struct superblock *sb_pop(struct superblock **list);
void *mm_malloc(size_t sz);
void mm_free(void *ptr);
int mm_init(void);

////////////////////////////////////////
// Debugging helpers / consistency checks:

#ifdef NDEBUG
#define check_superblock(x, y, z) ((void)(x))
#else
void check_superblock(struct superblock *sb, int sc, struct superblock **bin) {
    assert(sb->sc == sc);
    assert(sb->bin == bin);

    if (bin) {
        // !(*bin) ==> !prev && !next
        assert(*bin || (!sb->prev && !sb->next));
    }

    if (sb->next) {
        assert(sb->next->prev == sb);
    }

    if (sb->prev) {
        assert(sb->prev->next == sb);
    } else if (bin) {
        assert(*bin == sb);
    }

    // Check freelist.
    int num_free = 0;
    struct freelist *block;
    for (block = sb->freelist; block; block = block->next) {
        num_free++;
    }
    if (sb->sc != EMPTY_SC) {
        assert(sb->used + num_free == max_blocks[sb->sc]);
    } else {
        assert(sb->used == 0);
    }
}
#endif

#ifdef NDEBUG
#define check_heap(x) ((void)(x))
#else
void check_heap(struct heap *heap) {
    int sc;
    for (sc = 0; sc < NSIZES; sc++) {
        int allocated = 0, used = 0;
        int bin;
        for (bin = 0; bin < NBINS; bin++) {
            struct superblock *sb;
            for (sb = heap->subheaps[sc].bins[bin]; sb != NULL; sb = sb->next) {
                check_superblock(sb, sc, &heap->subheaps[sc].bins[bin]);
                assert(sb->used != 0);
                assert(sb->used != max_blocks[sc]);
                assert(sb->used / (float)max_blocks[sc] >= (bin) / (float)NBINS);
                assert(sb->used / (float)max_blocks[sc] <= (bin + 1) / (float)NBINS);
                allocated += max_blocks[sc];
                used += sb->used;
            }
        }
        struct superblock *sb;
        for (sb = heap->subheaps[sc].full_bin; sb != NULL; sb = sb->next) {
            check_superblock(sb, sc, &heap->subheaps[sc].full_bin);
            assert(sb->used == max_blocks[sc]);
            allocated += max_blocks[sc];
            used += max_blocks[sc];
        }
        assert(heap->subheaps[sc].allocated == allocated);
        assert(heap->subheaps[sc].used == used);
    }
    int num_empties = 0;
    struct superblock *sb;
    for (sb = heap->empty_bin; sb != NULL; sb = sb->next) {
        check_superblock(sb, EMPTY_SC, &heap->empty_bin);
        assert(sb->used == 0);
        num_empties++;
    }
    if (heap != global_heap) {
        assert(num_empties <= K_THRESH);
    }
}
#endif

#ifdef NDEBUG
#define check_heaps()
#else
void check_heaps() {
    check_heap(global_heap);
    int cpu;
    for (cpu = 0; cpu < num_cpus; cpu++) {
        check_heap(&cpu_heaps[cpu]);
    }
}
#endif

int heap_is_empty(struct heap *heap) {
    if (heap->num_empties != 0) {
        return 0;
    }
    int sc;
    for (sc = 0; sc < NSIZES; sc++) {
        if (heap->subheaps[sc].allocated != 0) {
            return 0;
        }
    }
    return 1;
}

void print_heap_stats(struct heap *heap) {
    if (heap_is_empty(heap)) {
        return;
    }
    printf("heap: %ld\n", heap - global_heap);
    printf("num_empties: %d\n", heap->num_empties);
    int sc;
    for (sc = 0; sc < NSIZES; sc++) {
        if (heap->subheaps[sc].allocated == 0) {
            continue;
        }
        printf("  sc: %d\n", sc);

        int num_full = 0;
        struct superblock *sb;
        for (sb = heap->subheaps[sc].full_bin; sb; sb = sb->next) {
            num_full++;
        }
        printf("  num_fulls: %d\n", num_full);

        int bin;
        for (bin = 0; bin < NBINS; bin++) {
            if (heap->subheaps[sc].bins[bin] == NULL) {
                continue;
            }
            printf("    bin: %d\n", bin);
            struct superblock *sb;
            for (sb = heap->subheaps[sc].bins[bin]; sb; sb = sb->next) {
                printf("      sb: %f%%\n", 100 * sb->used / (float) max_blocks[sb->sc]);
            }
        }
    }
}

#ifdef NDEBUG
void print_stats() {}
#else
void print_stats() {
    printf("Checking heaps...");
    check_heaps();
    printf("good.\n");

    print_heap_stats(global_heap);
    int cpu;
    for (cpu = 0; cpu < num_cpus; cpu++) {
        print_heap_stats(&cpu_heaps[cpu]);
    }
}
#endif

////////////////////////////////////////

/*
 * Initialize an empty heap.
 */
void init_heap(struct heap *heap) {
    int sc;
    for (sc = 0; sc < NSIZES; sc++) {
        init_subheap(&heap->subheaps[sc]);
    }
    heap->empty_bin = NULL;
    heap->num_empties = 0;
    pthread_mutex_init(&heap->empties_lock, NULL);
}

/*
 * Initialize an empty subheap.
 */
void init_subheap(struct subheap *subheap) {
    int bin;
    for (bin = 0; bin < NBINS; bin++) {
        subheap->bins[bin] = NULL;
    }
    subheap->full_bin = NULL;
    subheap->allocated = 0;
    subheap->used = 0;
    pthread_mutex_init(&subheap->lock, NULL);
}

/*
 * Initialize a superblock of a given size class.
 */
void init_superblock(struct superblock *sb, signed char sc) {
    assert(sb != NULL);
    assert(sc >= 0);

    sb->sc = sc;
    sb->used = 0;
    sb->bin = NULL;
    sb->prev = NULL;
    sb->next = NULL;

    // Initialize the free list of the superblock.
    sb->freelist = (struct freelist *) ((char *)sb + sizeof(struct superblock));

    struct freelist *freelist = sb->freelist;
    int mblocks = max_blocks[sc];
    int i;
    for (i = 0; i < mblocks - 1; i++) {
        freelist->next = (struct freelist *) ((char *)freelist + sizes[sc]);
        freelist = freelist->next;
    }
    freelist->next = NULL;

    check_superblock(sb, sc, NULL);
}

/*
 * Rounds the given size up to the nearest size class boundary, and returns its
 * index.
 *
 * This function is optimized for memory allocation requests that fall into the
 * 8, 16, 32 and 64-byte size classes.
 */
signed char get_size_class(size_t size) {
    assert(size >= 0);
    assert(size <= sizes[NSIZES-1]);

    register char cmp_lo = sizes[1] < size;
    register char cmp_hi = size <= sizes[2];

    if (cmp_lo && cmp_hi) {             // 16 < size <= 32
        return 2;

    } else if (cmp_hi) {                // size <= 16
        if (size <= sizes[0]) {         // size <= 8
            return 0;
        } else {                        // 8 < size <= 16
            return 1;
        }

    } else {                            // 32 < size
        if (size <= sizes[3]) {         // 32 < size <= 64
            return 3;
        } else if (size <= sizes[4]) {  // 64 < size <= 128
            return 4;
        } else if (size <= sizes[5]) {  // 128 < size <= 265
            return 5;
        } else if (size <= sizes[6]) {  // 265 < size <= 450
            return 6;
        }
    }

    assert(0);
    return -1;
}

/*
 * Remove a superblock from its containing bin.
 */
void sb_remove(struct superblock *sb) {
    assert(sb != NULL);
    assert(sb->bin != NULL);

    if (sb->prev == NULL) {
        sb_pop(sb->bin);
    } else if (sb->next == NULL) {
        sb->prev->next = NULL;
    } else {
        sb->prev->next = sb->next;
        sb->next->prev = sb->prev;
    }

    sb->bin = NULL;
    sb->next = NULL;
    sb->prev = NULL;
}

/*
 * Inserts the given superblock at the head of the specified bin.
 */
void sb_push(struct superblock **bin, struct superblock *sb) {
    assert(bin != NULL);
    assert(sb != NULL);

    // Don't try to push a superblock belonging to someone else!
    assert(sb->bin == NULL);

    // Push sb to its new bin.
    sb->bin = bin;
    sb->next = *bin;
    sb->prev = NULL;

    if (*bin != NULL) {
        (*bin)->prev = sb;
    }
    *bin = sb;
}

/*
 * Removes and returns the superblock at the head of the given bin.
 */
struct superblock *sb_pop(struct superblock **bin) {
    assert(bin != NULL);

    if (*bin == NULL) {
        // Empty list, return NULL immediately.
        return NULL;
    }

    struct superblock *sb = *bin;

    if (sb->next != NULL) {
        sb->next->prev = NULL;
    }

    *bin = sb->next;

    sb->bin = NULL;
    sb->next = NULL;
    sb->prev = NULL;

    return sb;
}

/*
 * Get the heap containing this superblock.
 */
struct heap *get_heap(struct superblock **bin) {
    assert(bin != NULL);
    return global_heap +
        ((char*)bin - (char*)global_heap) / sizeof(struct heap);
}

/*
 * Get the appropriate fullness bin index for this superblock.
 */
int appropriate_bin(struct superblock *sb) {
    return NBINS * sb->used / max_blocks[sb->sc];
}

/*
 * Allocate as many superblock sizes worth of memory as are needed to
 * fit the requested object size.
 *
 * Set a header field indicating that this is a "large block" and not
 * just any old superblock.
 */
void *alloc_large_block(size_t sz) {
    int num_superblocks = SB_ALIGN_UP(sz + sizeof(struct largeblock)) / SB_SIZE;

    pthread_mutex_lock(&sbrk_lock);
    struct largeblock *largeblock = SBRK(num_superblocks * SB_SIZE);
    pthread_mutex_unlock(&sbrk_lock);

    largeblock->sc = LARGE_SC;
    largeblock->num_superblocks = num_superblocks;

    return largeblock + 1;
}

/*
 * Free/recycle a "large block" by pushing its superblocks to the global heap's
 * empty-bin, to be assigned a size class later on.
 */
void free_large_block(struct largeblock *largeblock) {
    int num_superblocks = largeblock->num_superblocks;

    int i;
    for (i = 0; i < num_superblocks; i++) {
        memset(largeblock, 0, SB_SIZE);

        pthread_mutex_lock(&global_heap->empties_lock);
        sb_push(&global_heap->empty_bin, (struct superblock *) largeblock);
        global_heap->num_empties++;
        pthread_mutex_unlock(&global_heap->empties_lock);

        largeblock = (struct largeblock *) ((char *) largeblock + SB_SIZE);
    }
}

////////////////////////////////////////

void *mm_malloc(size_t sz) {
    // Handle requests for "large" amounts of memory.
    if (sz > sizes[NSIZES-1]) {
        #ifndef NDEBUG
        printf("Large malloc: %lu\n", sz);
        #endif

        return alloc_large_block(sz);
    }

    int cpu = sched_getcpu();
    signed char sc = get_size_class(sz);

    pthread_mutex_lock(&cpu_heaps[cpu].subheaps[sc].lock);

    // Find a superblock that has a free block. Prefer fuller superblocks.
    struct superblock *sb = NULL;
    int bin;
    for (bin = NBINS - 1; bin >= 0; bin--) {
        sb = cpu_heaps[cpu].subheaps[sc].bins[bin];
        if (sb != NULL) {
            break;
        }
    }

    pthread_mutex_unlock(&cpu_heaps[cpu].subheaps[sc].lock);

    // Aquire a superblock to add to the local heap.
    if (sb == NULL) {
        // Recycle an empty superblock for use with this size class if possible.
        pthread_mutex_lock(&cpu_heaps[cpu].empties_lock);
        sb = sb_pop(&cpu_heaps[cpu].empty_bin);
        if (sb != NULL) {
            cpu_heaps[cpu].num_empties--;
        }
        pthread_mutex_unlock(&cpu_heaps[cpu].empties_lock);
        if (sb != NULL) {
            init_superblock(sb, sc); // Initialize freelist.
        }

        // Grab a superblock from the global heap if possible.
        if (sb == NULL) {
            pthread_mutex_lock(&global_heap->subheaps[sc].lock);
            sb = sb_pop(&global_heap->subheaps[sc].bins[0]);
            if (sb != NULL) {
                global_heap->subheaps[sc].allocated -= max_blocks[sc];
                global_heap->subheaps[sc].used -= sb->used;
            }
            pthread_mutex_unlock(&global_heap->subheaps[sc].lock);
        }

        // Grab from global_heap's empties if possible.
        if (sb == NULL) {
            pthread_mutex_lock(&global_heap->empties_lock);
            sb = sb_pop(&global_heap->empty_bin);
            if (sb != NULL) {
                global_heap->num_empties--;
            }
            pthread_mutex_unlock(&global_heap->empties_lock);
            if (sb != NULL) {
                init_superblock(sb, sc); // Initialize freelist.
            }
        }

        // Allocate a new superblock if neccessary.
        if (sb == NULL) {
            pthread_mutex_lock(&sbrk_lock);
            sb = (struct superblock *) SBRK(SB_SIZE);
            pthread_mutex_unlock(&sbrk_lock);
            init_superblock(sb, sc);
        }

        pthread_mutex_lock(&cpu_heaps[cpu].subheaps[sc].lock);
        sb_push(&cpu_heaps[cpu].subheaps[sc].bins[0], sb);
        cpu_heaps[cpu].subheaps[sc].allocated += max_blocks[sc];
        cpu_heaps[cpu].subheaps[sc].used += sb->used;
    } else {
        pthread_mutex_lock(&cpu_heaps[cpu].subheaps[sc].lock);
    }

    // Allocate a single block within the superblock.
    struct freelist *block = sb->freelist;
    sb->freelist = block->next;
    sb->used++;
    cpu_heaps[cpu].subheaps[sc].used++;

    // Adjust fullness bin.
    if (sb->used == max_blocks[sc]) {
        sb_remove(sb);
        sb_push(&cpu_heaps[cpu].subheaps[sc].full_bin, sb);
    } else {
        int bin = appropriate_bin(sb);

        if (sb->bin != &cpu_heaps[cpu].subheaps[sc].bins[bin]) {
            sb_remove(sb);
            sb_push(&cpu_heaps[cpu].subheaps[sc].bins[bin], sb);
        }
    }

    pthread_mutex_unlock(&cpu_heaps[cpu].subheaps[sc].lock);

	return block;
}

void mm_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    struct superblock *sb = (struct superblock *) SB_ALIGN(ptr);
    signed char sc = sb->sc;

    // Handle frees of "large" blocks.
    if (sc == LARGE_SC) {
        free_large_block((struct largeblock *) sb);
        return;
    }

    // Try to acquire the heap lock for this superblock,
    // repeating if the superblock moves between heaps before we
    // have a chance to acquire the lock.
    struct heap *heap = NULL;
    while (1) {
        if (sb->bin != NULL) {
            heap = get_heap(sb->bin);
            pthread_mutex_lock(&heap->subheaps[sc].lock);
            if (sb->bin != NULL && get_heap(sb->bin) == heap) {
                break; // Success!
            }
            pthread_mutex_unlock(&heap->subheaps[sc].lock);
        }

        // Memory barrier to prevent compile-time reordering/optimizations
        // from causing this loop to never terminate.
        asm volatile("" ::: "memory");
    }

    struct freelist *freed_block = (struct freelist *) ptr;

    freed_block->next = sb->freelist;
    sb->freelist = freed_block;

    sb->used--;
    heap->subheaps[sc].used--;

    // Adjust fullness bin. Note that if a superblock gets pushed to a bin
    // it was already in, this orders it at the front of its bin. And this
    // is desirable for locality.
    // (The "move-to-front" heuristic mentioned in the paper).
    if (sb->used != 0) {
        int bin = appropriate_bin(sb);

        sb_remove(sb);
        sb_push(&heap->subheaps[sc].bins[bin], sb);
    } else {
        // Recycling.
        sb->sc = EMPTY_SC;
        sb->freelist = NULL;
        heap->subheaps[sc].allocated -= max_blocks[sc];
        if (heap != global_heap && heap->num_empties >= K_THRESH) {
            // Pseudo-reclamation. (Sending empty sb's to the global heap).
            sb_remove(sb);
            pthread_mutex_lock(&global_heap->empties_lock);
            sb_push(&global_heap->empty_bin, sb);
            global_heap->num_empties++;
            pthread_mutex_unlock(&global_heap->empties_lock);
        } else {
            // Regular recycling.
            sb_remove(sb);
            pthread_mutex_lock(&heap->empties_lock);
            sb_push(&heap->empty_bin, sb);
            heap->num_empties++;
            pthread_mutex_unlock(&heap->empties_lock);
        }
    }

    if (heap == global_heap) {
        pthread_mutex_unlock(&heap->subheaps[sc].lock);
        return;
    }

    // Reclamation.
    struct subheap *subheap = &heap->subheaps[sc];
    if (subheap->used < subheap->allocated - K_THRESH * max_blocks[sc]
            && NBINS * subheap->used < subheap->allocated) {
        // The invariant should ensure that there is a sb in this bin.
        assert(subheap->bins[0] != NULL);

        sb = sb_pop(&subheap->bins[0]);
        subheap->allocated -= max_blocks[sc];
        subheap->used -= sb->used;

        pthread_mutex_lock(&global_heap->subheaps[sc].lock);
        sb_push(&global_heap->subheaps[sc].bins[0], sb);
        global_heap->subheaps[sc].allocated += max_blocks[sc];
        global_heap->subheaps[sc].used += sb->used;
        pthread_mutex_unlock(&global_heap->subheaps[sc].lock);
    }

    pthread_mutex_unlock(&heap->subheaps[sc].lock);
}

int mm_init(void) {
    // Init memory and align heap to the nearest multiple of superblock size.
    if (mem_init()) {
        return -1;
    }
    dseg_lo = (char *) SB_ALIGN_UP(dseg_lo);
    dseg_hi = dseg_lo - 1;

    // Init globals.

    num_cpus = getNumProcessors();

    int sc;
    for (sc = 0; sc < NSIZES; sc++) {
        max_blocks[sc] = (SB_SIZE - sizeof(struct superblock)) / sizes[sc];
    }

    global_heap = SBRK(sizeof(struct heap) * (num_cpus + 1));
    cpu_heaps = global_heap + 1;

    init_heap(global_heap);
    int cpu;
    for (cpu = 0; cpu < num_cpus; cpu++) {
        init_heap(&cpu_heaps[cpu]);
    }

    // Sanity checks:

    // Do our size classes make sense with our SB size?
    // (The largest size class should be able to fit at least two blocks in
    // a superblock.)
    assert(2 * sizes[NSIZES-1] <= SB_SIZE - sizeof(struct superblock));

    check_heaps();

    return 0;
}
