// Lab 6 Task 1.1 microbenchmark: page-granular random pointer chase over > 8 GB.
//
// One 8-byte node per 4 KB page, all nodes linked into a single random cycle
// (Sattolo's algorithm), so the chase visits every page once per lap in random
// order. Each step is ONE dependent load whose address comes from the previous
// load:   p = *p;
// so there is no memory-level parallelism and the full translation latency
// (TLB miss -> page walk) of every step is exposed. Consecutive steps land on
// unrelated pages, so with a 512-entry L2 TLB (2 MB of reach with 4 KB pages,
// 1 GB with 2 MB pages) nearly every step misses: L2 TLB MPKI >> 10.
//
// Within its page, each node sits at a pseudo-random cache-line offset, so the
// nodes spread over all cache sets instead of all mapping to line 0 of a page.
// The array is 2 MB aligned so 2 MB pages in the simulated OS align with it.
//
// Usage: microbench [footprint_GiB=8.5] [steps=25000000] [seed=42]
// Only the chase loop is inside SimRoiStart()/SimRoiEnd(); the trace is recorded
// with record-trace --roi (see record_microbench_trace.sh).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <sys/mman.h>
#include "sim_api.h"

static const uint64_t PAGE = 4096;
static const uint64_t LINE = 64;
static const uint64_t HUGE_ALIGN = 2ull << 20;

static uint8_t *g_base = nullptr;
static uint64_t g_pages = 0;

static inline uint64_t *nodeOf(uint64_t page)
{
    // pseudo-random line within the page (Fibonacci hashing of the page number)
    uint64_t line = (page * 0x9E3779B97F4A7C15ull) >> 58;   // 0..63
    return reinterpret_cast<uint64_t *>(g_base + page * PAGE + line * LINE);
}

static void init(double gib, uint64_t seed)
{
    const uint64_t bytes = static_cast<uint64_t>(gib * (1ull << 30)) & ~(PAGE - 1);
    g_pages = bytes / PAGE;

    void *raw = mmap(nullptr, bytes + HUGE_ALIGN, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); exit(1); }
    g_base = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(raw) + HUGE_ALIGN - 1) & ~(HUGE_ALIGN - 1));

    // Single random cycle over all pages (Sattolo): order[i] -> order[i+1] -> ... -> order[0]
    std::vector<uint32_t> order(g_pages);
    for (uint64_t i = 0; i < g_pages; i++) order[i] = static_cast<uint32_t>(i);
    std::mt19937_64 rng(seed);
    for (uint64_t i = g_pages - 1; i > 0; i--) {
        uint64_t j = rng() % i;            // j < i  (Sattolo => one single cycle)
        std::swap(order[i], order[j]);
    }
    // successor of page order[i] is order[i+1]; write in page order so the
    // first touch of the array is sequential (fast, swap-friendly)
    std::vector<uint32_t> next(g_pages);
    for (uint64_t i = 0; i < g_pages; i++) next[order[i]] = order[(i + 1) % g_pages];
    for (uint64_t p = 0; p < g_pages; p++)
        *nodeOf(p) = reinterpret_cast<uint64_t>(nodeOf(next[p]));

    fprintf(stderr, "[microbench] footprint %.2f GiB = %lu pages of 4 KB (%lu x 2 MB), base %p\n",
            bytes / double(1ull << 30), (unsigned long)g_pages, (unsigned long)(bytes / HUGE_ALIGN), (void *)g_base);
}

int main(int argc, char **argv)
{
    const double gib = argc > 1 ? atof(argv[1]) : 8.5;
    const uint64_t steps = argc > 2 ? strtoull(argv[2], nullptr, 10) : 25000000ull;
    const uint64_t seed = argc > 3 ? strtoull(argv[3], nullptr, 10) : 42;

    init(gib, seed);
    fprintf(stderr, "[microbench] chasing %lu steps\n", (unsigned long)steps);

    uint64_t *p = nodeOf(0);
    SimRoiStart();
    for (uint64_t i = 0; i < steps; i++)
        p = reinterpret_cast<uint64_t *>(*p);        // dependent load: address = previous value
    SimRoiEnd();

    // use the result so the loop cannot be optimised away
    printf("[microbench] done, final node %p\n", (void *)p);
    return 0;
}
