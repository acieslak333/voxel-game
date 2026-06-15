// Standalone unit test for SpanAllocator — compile without the rest of the
// engine (no Vulkan, no GLFW, no CMake). Run directly (from repo root):
//
//   g++ -std=c++20 -O2 -Wall -Wextra
//       -o /tmp/span_test
//       src/render/SpanAllocator.cpp
//       tests/span_allocator_test.cpp
//   && /tmp/span_test
//
// Returns 0 on success, non-zero on first failure.

#include "render/SpanAllocator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using vg::SpanAllocator;

// ---------------------------------------------------------------------------
// Minimal test harness — prints the failing expression and exits non-zero.
// ---------------------------------------------------------------------------
static int  g_failures = 0;
static void fail(const char* file, int line, const char* expr) {
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    ++g_failures;
}
#define CHECK(expr)                                         \
    do {                                                    \
        if (!(expr)) { fail(__FILE__, __LINE__, #expr); }   \
    } while (false)

// Invariant helper: used + freeBytes == capacity.
static void checkInvariant(const SpanAllocator& sa, const char* where) {
    if (sa.used() + sa.freeBytes() != sa.capacity()) {
        std::fprintf(stderr, "INVARIANT BROKEN at %s: used=%llu freeBytes=%llu capacity=%llu\n",
                     where,
                     (unsigned long long)sa.used(),
                     (unsigned long long)sa.freeBytes(),
                     (unsigned long long)sa.capacity());
        ++g_failures;
    }
}

// Check that `offset` is aligned to `alignment`.
static bool isAligned(uint64_t offset, uint64_t alignment) {
    return alignment == 0 || (offset & (alignment - 1u)) == 0;
}

// ---------------------------------------------------------------------------
// Test 1 — simple alloc/free round trips; returned offsets are aligned.
// ---------------------------------------------------------------------------
static void test_simple_roundtrip() {
    SpanAllocator sa(1024);
    checkInvariant(sa, "T1-init");
    CHECK(sa.freeBytes() == 1024);
    CHECK(sa.used()      == 0);

    uint64_t a = sa.allocate(64, 16);
    CHECK(a != SpanAllocator::kInvalid);
    CHECK(isAligned(a, 16));
    checkInvariant(sa, "T1-after-alloc");

    uint64_t b = sa.allocate(128, 32);
    CHECK(b != SpanAllocator::kInvalid);
    CHECK(isAligned(b, 32));
    CHECK(b != a);
    checkInvariant(sa, "T1-after-alloc2");

    sa.free(a);
    checkInvariant(sa, "T1-after-free-a");
    sa.free(b);
    checkInvariant(sa, "T1-after-free-b");

    // After freeing everything the arena should be fully reclaimed.
    CHECK(sa.used()      == 0);
    CHECK(sa.freeBytes() == 1024);
    CHECK(sa.largestFreeBlock() == 1024);

    std::printf("PASS test_simple_roundtrip\n");
}

// ---------------------------------------------------------------------------
// Test 2 — alignment padding: odd-size allocations followed by aligned ones;
// verify returned offsets are aligned and no bytes leak.
// ---------------------------------------------------------------------------
static void test_alignment_padding() {
    // Capacity is intentionally not a multiple of any large power of two so
    // that natural placement forces non-trivial padding on some requests.
    SpanAllocator sa(1000);

    // Alloc 1 byte to push the cursor to offset 1.
    uint64_t a0 = sa.allocate(1, 1);
    CHECK(a0 == 0);
    checkInvariant(sa, "T2-a0");

    // Alloc 64-byte aligned: next free range starts at 1, so 63 bytes of pad.
    uint64_t a1 = sa.allocate(64, 64);
    CHECK(a1 != SpanAllocator::kInvalid);
    CHECK(isAligned(a1, 64));
    CHECK(a1 == 64); // should land at 64
    checkInvariant(sa, "T2-a1");

    // Alloc 3 bytes (no alignment needed).
    uint64_t a2 = sa.allocate(3, 1);
    CHECK(a2 == 128); // right after the 64-byte block [64,128)
    checkInvariant(sa, "T2-a2");

    // Alloc 256-byte aligned: free range starts at 131; expect offset 256.
    uint64_t a3 = sa.allocate(256, 256);
    CHECK(a3 != SpanAllocator::kInvalid);
    CHECK(isAligned(a3, 256));
    CHECK(a3 == 256);
    checkInvariant(sa, "T2-a3");

    // Free all and confirm full coalesce.
    sa.free(a0);
    sa.free(a1);
    sa.free(a2);
    sa.free(a3);
    checkInvariant(sa, "T2-after-all-free");
    CHECK(sa.used() == 0);
    CHECK(sa.largestFreeBlock() == 1000);

    std::printf("PASS test_alignment_padding\n");
}

// ---------------------------------------------------------------------------
// Test 3 — fragmentation + coalescing: allocate many spans, free in
// scrambled order, assert full coalesce at the end.
// ---------------------------------------------------------------------------
static void test_fragmentation_coalescing() {
    constexpr uint64_t kCap    = 65536;
    constexpr uint64_t kAllocs = 32;
    constexpr uint64_t kSize   = kCap / kAllocs; // 2048 each, exact fit

    SpanAllocator sa(kCap);
    std::vector<uint64_t> offsets;
    offsets.reserve(kAllocs);

    for (uint64_t i = 0; i < kAllocs; ++i) {
        uint64_t off = sa.allocate(kSize, 1);
        CHECK(off != SpanAllocator::kInvalid);
        offsets.push_back(off);
        checkInvariant(sa, "T3-alloc");
    }
    CHECK(sa.freeBytes() == 0);
    CHECK(sa.largestFreeBlock() == 0);

    // Scramble the free order using a fixed permutation (odd indices first, then even).
    std::vector<uint64_t> freeOrder;
    freeOrder.reserve(kAllocs);
    for (uint64_t i = 1; i < kAllocs; i += 2) { freeOrder.push_back(offsets[i]); }
    for (uint64_t i = 0; i < kAllocs; i += 2) { freeOrder.push_back(offsets[i]); }

    for (uint64_t off : freeOrder) {
        sa.free(off);
        checkInvariant(sa, "T3-free");
    }

    // Full coalesce: one range covering the entire arena.
    CHECK(sa.used() == 0);
    CHECK(sa.largestFreeBlock() == kCap);

    std::printf("PASS test_fragmentation_coalescing\n");
}

// ---------------------------------------------------------------------------
// Test 4 — out-of-space returns kInvalid and does not corrupt state; a
// subsequent free of a live span still works correctly.
// ---------------------------------------------------------------------------
static void test_out_of_space() {
    SpanAllocator sa(512);

    uint64_t a = sa.allocate(400, 1);
    CHECK(a != SpanAllocator::kInvalid);
    checkInvariant(sa, "T4-a");

    // 112 bytes remain; requesting 200 must fail.
    uint64_t b = sa.allocate(200, 1);
    CHECK(b == SpanAllocator::kInvalid);
    checkInvariant(sa, "T4-fail");           // state must be unmodified
    CHECK(sa.freeBytes() == 512 - 400);      // exactly 112 bytes still free

    // A smaller allocation should still succeed.
    uint64_t c = sa.allocate(100, 1);
    CHECK(c != SpanAllocator::kInvalid);
    checkInvariant(sa, "T4-c");

    // Free the live spans; allocator should be clean.
    sa.free(a);
    sa.free(c);
    checkInvariant(sa, "T4-cleanup");
    CHECK(sa.used() == 0);
    CHECK(sa.largestFreeBlock() == 512);

    std::printf("PASS test_out_of_space\n");
}

// ---------------------------------------------------------------------------
// Test 5 — kInvalid returned when alignment padding alone exhausts the range.
// ---------------------------------------------------------------------------
static void test_alignment_exhausts_range() {
    // Arena of 128 bytes. Eat the first 1 byte so next free range starts at 1.
    SpanAllocator sa(128);
    uint64_t a0 = sa.allocate(1, 1);
    CHECK(a0 == 0);

    // Request 128-byte block aligned to 128 — needs 127 bytes of pad + 128 data
    // = 255 bytes, but only 127 bytes remain. Must return kInvalid.
    uint64_t a1 = sa.allocate(128, 128);
    CHECK(a1 == SpanAllocator::kInvalid);
    checkInvariant(sa, "T5-fail");

    sa.free(a0);
    CHECK(sa.largestFreeBlock() == 128);

    std::printf("PASS test_alignment_exhausts_range\n");
}

// ---------------------------------------------------------------------------
// Test 6 — reset() discards all state including live spans (used after reset).
// ---------------------------------------------------------------------------
static void test_reset() {
    SpanAllocator sa(256);
    uint64_t a = sa.allocate(100, 1);
    CHECK(a != SpanAllocator::kInvalid);

    // Reset to a different capacity — live spans are intentionally abandoned.
    sa.reset(1024);
    CHECK(sa.capacity()        == 1024);
    CHECK(sa.used()            == 0);
    CHECK(sa.freeBytes()       == 1024);
    CHECK(sa.largestFreeBlock() == 1024);
    checkInvariant(sa, "T6-after-reset");

    // New allocations from the clean slate.
    uint64_t b = sa.allocate(512, 256);
    CHECK(b != SpanAllocator::kInvalid);
    CHECK(isAligned(b, 256));
    checkInvariant(sa, "T6-alloc-after-reset");
    sa.free(b);
    CHECK(sa.largestFreeBlock() == 1024);

    std::printf("PASS test_reset\n");
}

// ---------------------------------------------------------------------------
// Test 7 — randomised fuzz (fixed seed, ~100k ops).
//
// At each step either allocate a random-sized, randomly-aligned block or free
// one of the live blocks. After every operation:
//   • used() + freeBytes() == capacity()              (invariant)
//   • every live offset is properly aligned            (alignment)
//   • no two live spans overlap                       (non-overlap)
// After draining all live spans: largestFreeBlock() == capacity().
// ---------------------------------------------------------------------------
static void test_fuzz() {
    constexpr uint64_t kCap    = 1u << 20; // 1 MiB arena
    constexpr int      kOps    = 100'000;
    constexpr uint32_t kSeed   = 0xDEAD'BEEFu;

    SpanAllocator sa(kCap);

    // live[aligned_offset] = {spanStart, spanSize, alignment_used}
    struct LiveEntry { uint64_t spanStart; uint64_t spanSize; uint64_t alignment; };
    std::unordered_map<uint64_t, LiveEntry> live;
    live.reserve(4096);

    std::mt19937 rng(kSeed);
    auto randRange = [&](uint64_t lo, uint64_t hi) -> uint64_t {
        return lo + rng() % (hi - lo + 1);
    };

    for (int op = 0; op < kOps; ++op) {
        const bool doFree = !live.empty() && (live.size() > 512 || (rng() & 1));

        if (doFree) {
            // Pick a random live entry and free it.
            auto it = live.begin();
            std::advance(it, rng() % live.size());
            const uint64_t off = it->first;
            live.erase(it);
            sa.free(off);
        } else {
            // Random size [1, 8192], random alignment from {1,2,4,8,16,32,64,128,256}.
            const uint64_t size = randRange(1, 8192);
            const uint64_t alignShift = rng() % 9; // 0..8
            const uint64_t alignment  = 1u << alignShift;
            const uint64_t off = sa.allocate(size, alignment);
            if (off != SpanAllocator::kInvalid) {
                live.emplace(off, LiveEntry{off, size, alignment});
            }
        }

        // --- Invariant check ---
        checkInvariant(sa, "fuzz-loop");

        // Alignment check for all live entries.
        for (const auto& [off, entry] : live) {
            if (!isAligned(off, entry.alignment)) {
                std::fprintf(stderr, "FUZZ: misaligned offset %llu (align %llu) at op %d\n",
                             (unsigned long long)off,
                             (unsigned long long)entry.alignment, op);
                ++g_failures;
            }
        }
    }

    // Non-overlap check: collect all live [start, end) intervals and sort.
    {
        struct Interval { uint64_t start; uint64_t end; };
        std::vector<Interval> intervals;
        intervals.reserve(live.size());
        for (const auto& [off, entry] : live) {
            intervals.push_back({off, off + entry.spanSize});
        }
        std::sort(intervals.begin(), intervals.end(),
                  [](const Interval& x, const Interval& y){ return x.start < y.start; });
        for (size_t i = 1; i < intervals.size(); ++i) {
            if (intervals[i].start < intervals[i-1].end) {
                std::fprintf(stderr, "FUZZ: overlapping spans [%llu,%llu) and [%llu,%llu)\n",
                             (unsigned long long)intervals[i-1].start,
                             (unsigned long long)intervals[i-1].end,
                             (unsigned long long)intervals[i].start,
                             (unsigned long long)intervals[i].end);
                ++g_failures;
            }
        }
    }

    // Drain everything and verify full coalesce.
    for (const auto& [off, entry] : live) {
        sa.free(off);
        checkInvariant(sa, "fuzz-drain");
    }
    live.clear();
    CHECK(sa.used() == 0);
    CHECK(sa.largestFreeBlock() == kCap);

    std::printf("PASS test_fuzz (%d ops, seed 0x%X)\n", kOps, kSeed);
}

// ---------------------------------------------------------------------------
int main() {
    test_simple_roundtrip();
    test_alignment_padding();
    test_fragmentation_coalescing();
    test_out_of_space();
    test_alignment_exhausts_range();
    test_reset();
    test_fuzz();

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", g_failures);
    return 1;
}
