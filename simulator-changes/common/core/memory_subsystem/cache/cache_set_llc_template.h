#ifndef CACHE_SET_LLC_TEMPLATE_H
#define CACHE_SET_LLC_TEMPLATE_H

#include "cache_set.h"
#include <vector>

// CompArch 2025 Lab 6, Task 1.3: PTE-aware RRIP for the LLC ("llc_template").
//
// Per-way age (RRPV): 0 = keep, max = evict. Data lines behave exactly like
// the baseline SRRIP LLC (insert at max-1, a hit decrements the age), so any
// difference comes from how page-table (PTE) lines are treated:
//
//  * Insert: PTE line requested by a walk the TLB<->LLC Bloom filter flagged
//            (translation recently evicted from the L2 TLB; the hint is tagged
//            on the line address, see pte_priority_hint.h) -> insert_pte_hint;
//            other PTE lines -> insert_pte; data -> insert_data.
//  * Hit:    PTE line -> pte_hit_age (default 0); data -> age-1 (as SRRIP).
//  * Victim: among lines at max age, prefer data over PTE lines.
//  * Anti-starvation: once a set holds >= max_pte_ways PTE lines, PTE lines
//            lose all protection (inserted like data, preferred as victims),
//            so data always keeps at least (assoc - max_pte_ways) ways.
//
// Config (optional) under <cache cfg>/llc_pte/: bits, insert_data, insert_pte,
// insert_pte_hint, pte_hit_age, max_pte_ways. Setting insert_pte =
// insert_pte_hint gives a type-only policy (no Bloom filter influence);
// setting everything to the data values gives plain SRRIP.
class CacheSetLLCTemplate : public CacheSet
{
   public:
      CacheSetLLCTemplate(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, bool is_tlb_set);
      ~CacheSetLLCTemplate() {}

      UInt32 getReplacementIndex(CacheCntlr *cntlr);
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      std::vector<UInt8> m_age;
      UInt8 m_max_age;
      UInt8 m_insert_data;
      UInt8 m_insert_pte;
      UInt8 m_insert_pte_hint;
      UInt8 m_pte_hit_age;
      UInt32 m_max_pte_ways;
      UInt32 m_hand;

      // Victim is chosen before CacheSet::insert() copies in the new block, so
      // its type is only known on the next call into this set.
      SInt32 m_pending_way;
      bool m_pending_hint;

      bool isPTE(UInt32 way) const;
      UInt32 countPTE() const;
      void resolvePendingInsert();

      // Aggregated over all LLC sets.
      static bool s_stats_registered;
      static UInt64 s_fills_pte, s_fills_pte_hint, s_fills_data;
      static UInt64 s_evict_pte, s_evict_data, s_hits_pte, s_hits_data, s_cap_reached;
};

#endif /* CACHE_SET_LLC_TEMPLATE_H */
