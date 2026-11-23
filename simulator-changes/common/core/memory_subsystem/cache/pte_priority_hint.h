#ifndef PTE_PRIORITY_HINT_H
#define PTE_PRIORITY_HINT_H

#include "fixed_types.h"
#include <vector>
#include <deque>
#include <unordered_set>
#include <mutex>

// CompArch 2025 Lab 6, Task 1.3: TLB <-> LLC co-design side structure.
//
// A small Bloom filter remembers the virtual pages that were recently evicted
// from the last-level (L2) TLB. When the hardware page-table walker starts a
// walk for a virtual address that hits in the filter, the translation is one
// that (a) was useful enough to be cached in the TLB before and (b) is
// currently missing from the TLB. The walker then tags its memory requests
// with a one-bit "priority" hint, which the LLC replacement policy
// (CacheSetLLCTemplate) uses to insert/retain those PTE lines more strongly.
//
// The filter has two banks that rotate every `epoch` insertions (the older
// bank is cleared), so "recently evicted" means within the last 1-2 epochs and
// the filter never saturates.
//
// Config (optional, defaults in brackets), section perf_model/pte_bloom:
//   bits   [65536]  bits per bank (2 banks -> 16 KB total at the default)
//   hashes [2]      hash functions
//   epoch  [4096]   insertions per bank before rotation
//
// The hint travels with the request *address*: the walker (core thread) records
// the PTE cache lines that a hinted walk requests (markHintedLine), and the LLC
// fill path (Sniper's sim thread) looks the filled line up (isHintedLine) and
// passes the result to the replacement policy via a thread-local "insert hint".
// (A first version used a global "walk in progress" flag; DRAM replies wake the
// core before the LLC fill happens on the sim thread, so that flag was racy.)
class PtePriorityHint
{
   public:
      static void init();                                   // idempotent; registers stats
      static void recordTlbEviction(IntPtr vaddr, int page_size_bits);
      static bool containsPage(IntPtr vaddr);               // 4KB or 2MB granularity
      static void beginWalk(IntPtr vaddr);
      static void endWalk();
      static bool inWalk() { return s_in_walk; }
      static bool walkHint() { return s_in_walk && s_walk_hint; }   // core thread only

      // Address-tagged hint (thread-safe)
      static void markHintedLine(IntPtr line_addr);           // core thread, during a hinted walk
      static bool isHintedLine(IntPtr line_addr);             // LLC fill path
      static void setInsertHint(bool h) { t_insert_hint = h; } // set by the LLC fill path, same thread
      static bool insertHint() { return t_insert_hint; }      // read by the LLC policy
      static void countFill(bool addr_hint, bool racy_flag_hint);

   private:
      static bool s_initialized;
      static bool s_in_walk;
      static bool s_walk_hint;
      static UInt32 s_bits, s_hashes, s_epoch;
      static UInt32 s_active;                               // index of the bank being filled
      static UInt64 s_inserts_in_epoch;
      static std::vector<std::vector<bool> > s_banks;

      static UInt64 s_stat_inserts, s_stat_rotations, s_stat_walks, s_stat_hinted_walks;
      static UInt64 s_stat_hinted_lines, s_stat_hinted_fills, s_stat_flag_mismatch;

      static std::mutex s_lines_lock;
      static std::deque<IntPtr> s_lines_fifo;
      static std::unordered_set<IntPtr> s_lines;
      static const size_t LINES_CAPACITY = 4096;
      static thread_local bool t_insert_hint;

      static UInt64 key(IntPtr vaddr, int page_size_bits);
      static UInt64 hash(UInt64 key, UInt32 i);
      static bool bankContains(UInt32 bank, UInt64 key);
};

#endif // PTE_PRIORITY_HINT_H
