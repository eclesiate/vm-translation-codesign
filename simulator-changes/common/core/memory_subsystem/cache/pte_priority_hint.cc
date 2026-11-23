#include "pte_priority_hint.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "log.h"

// CompArch 2025 Lab 6, Task 1.3 (see header).

bool PtePriorityHint::s_initialized = false;
bool PtePriorityHint::s_in_walk = false;
bool PtePriorityHint::s_walk_hint = false;
UInt32 PtePriorityHint::s_bits = 65536;
UInt32 PtePriorityHint::s_hashes = 2;
UInt32 PtePriorityHint::s_epoch = 4096;
UInt32 PtePriorityHint::s_active = 0;
UInt64 PtePriorityHint::s_inserts_in_epoch = 0;
std::vector<std::vector<bool> > PtePriorityHint::s_banks;
UInt64 PtePriorityHint::s_stat_inserts = 0;
UInt64 PtePriorityHint::s_stat_rotations = 0;
UInt64 PtePriorityHint::s_stat_walks = 0;
UInt64 PtePriorityHint::s_stat_hinted_walks = 0;
UInt64 PtePriorityHint::s_stat_hinted_lines = 0;
UInt64 PtePriorityHint::s_stat_hinted_fills = 0;
UInt64 PtePriorityHint::s_stat_flag_mismatch = 0;
std::mutex PtePriorityHint::s_lines_lock;
std::deque<IntPtr> PtePriorityHint::s_lines_fifo;
std::unordered_set<IntPtr> PtePriorityHint::s_lines;
thread_local bool PtePriorityHint::t_insert_hint = false;

namespace
{
   SInt64 cfgInt(const String &key, SInt64 dflt)
   {
      if (Sim() && Sim()->getCfg()->hasKey(key))
         return Sim()->getCfg()->getInt(key);
      return dflt;
   }
}

void
PtePriorityHint::init()
{
   if (s_initialized)
      return;
   s_initialized = true;
   s_bits = std::max<SInt64>(cfgInt("perf_model/pte_bloom/bits", 65536), 64);
   s_hashes = std::max<SInt64>(cfgInt("perf_model/pte_bloom/hashes", 2), 1);
   s_epoch = std::max<SInt64>(cfgInt("perf_model/pte_bloom/epoch", 4096), 1);
   s_banks.assign(2, std::vector<bool>(s_bits, false));

   registerStatsMetric("pte_bloom", 0, "inserts", &s_stat_inserts);
   registerStatsMetric("pte_bloom", 0, "rotations", &s_stat_rotations);
   registerStatsMetric("pte_bloom", 0, "walks", &s_stat_walks);
   registerStatsMetric("pte_bloom", 0, "hinted_walks", &s_stat_hinted_walks);
   registerStatsMetric("pte_bloom", 0, "hinted_lines", &s_stat_hinted_lines);
   registerStatsMetric("pte_bloom", 0, "hinted_llc_fills", &s_stat_hinted_fills);
   registerStatsMetric("pte_bloom", 0, "old_flag_mismatch", &s_stat_flag_mismatch);
}

UInt64
PtePriorityHint::key(IntPtr vaddr, int page_size_bits)
{
   // VPN plus page size, so a 4KB and a 2MB translation never alias by construction.
   return (UInt64(vaddr) >> page_size_bits) ^ (UInt64(page_size_bits) << 56);
}

UInt64
PtePriorityHint::hash(UInt64 k, UInt32 i)
{
   // splitmix64 finaliser with a per-function seed: cheap, well mixed.
   UInt64 z = k + 0x9e3779b97f4a7c15ULL * (i + 1);
   z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
   z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
   z = z ^ (z >> 31);
   return z % s_bits;
}

bool
PtePriorityHint::bankContains(UInt32 bank, UInt64 k)
{
   for (UInt32 i = 0; i < s_hashes; i++)
      if (!s_banks[bank][hash(k, i)])
         return false;
   return true;
}

void
PtePriorityHint::recordTlbEviction(IntPtr vaddr, int page_size_bits)
{
   init();
   const UInt64 k = key(vaddr, page_size_bits);
   for (UInt32 i = 0; i < s_hashes; i++)
      s_banks[s_active][hash(k, i)] = true;
   s_stat_inserts++;

   if (++s_inserts_in_epoch >= s_epoch)
   {
      // Rotate: the older bank is cleared and becomes the active one.
      s_active ^= 1;
      std::fill(s_banks[s_active].begin(), s_banks[s_active].end(), false);
      s_inserts_in_epoch = 0;
      s_stat_rotations++;
   }
}

bool
PtePriorityHint::containsPage(IntPtr vaddr)
{
   init();
   const int sizes[2] = {12, 21};
   for (int s = 0; s < 2; s++)
   {
      const UInt64 k = key(vaddr, sizes[s]);
      if (bankContains(0, k) || bankContains(1, k))
         return true;
   }
   return false;
}

void
PtePriorityHint::beginWalk(IntPtr vaddr)
{
   s_walk_hint = containsPage(vaddr);
   s_in_walk = true;
   s_stat_walks++;
   if (s_walk_hint)
      s_stat_hinted_walks++;
}

void
PtePriorityHint::endWalk()
{
   s_in_walk = false;
   s_walk_hint = false;
}

void
PtePriorityHint::markHintedLine(IntPtr line_addr)
{
   line_addr &= ~IntPtr(63);
   std::lock_guard<std::mutex> g(s_lines_lock);
   if (s_lines.insert(line_addr).second)
   {
      s_lines_fifo.push_back(line_addr);
      s_stat_hinted_lines++;
      if (s_lines_fifo.size() > LINES_CAPACITY)
      {
         s_lines.erase(s_lines_fifo.front());
         s_lines_fifo.pop_front();
      }
   }
}

bool
PtePriorityHint::isHintedLine(IntPtr line_addr)
{
   line_addr &= ~IntPtr(63);
   std::lock_guard<std::mutex> g(s_lines_lock);
   return s_lines.count(line_addr) != 0;
}

void
PtePriorityHint::countFill(bool addr_hint, bool racy_flag_hint)
{
   // Called for every PAGE_TABLE fill into the LLC: how often would the old
   // global walk flag have given a different answer than the address tag?
   if (addr_hint) s_stat_hinted_fills++;
   if (addr_hint != racy_flag_hint) s_stat_flag_mismatch++;
}
