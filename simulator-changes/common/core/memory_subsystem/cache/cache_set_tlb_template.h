#ifndef CACHE_SET_TLB_TEMPLATE_H
#define CACHE_SET_TLB_TEMPLATE_H

#include "cache_set.h"
#include <vector>

// CompArch 2025 Lab 6, Task 1.2: page-size-aware SRRIP for the L2 TLB ("tlb_template").
//
// Per-way state: an RRPV (re-reference prediction value, 0 = keep, max = evict)
// plus an aging credit used to make 2MB entries age slower.
//
//  * Insert: 4KB entries at insert_4k (default max-1), 2MB at insert_2m (default 1).
//  * Hit:    RRPV = 0 (hit priority).
//  * Victim: first way with RRPV == max; while protection is active, a 4KB way
//            at max is preferred over a 2MB way at max.
//  * Aging:  if no way is at max, every 4KB entry ages by 1; protected 2MB
//            entries age by 1 only every huge_aging_period aging rounds.
//  * Anti-starvation: protection is disabled for a set holding more than
//            max_huge_ways 2MB entries, so 4KB translations always keep room.
//            Unused 2MB entries still reach max and get evicted (just slower).
//
// Config (all optional, under <tlb cfg>/psa/): bits, insert_4k, insert_2m,
// huge_aging_period, max_huge_ways.  huge_aging_period = 1 and insert_2m =
// insert_4k turns the policy into plain SRRIP.
class CacheSetTLBTemplate : public CacheSet
{
   public:
      CacheSetTLBTemplate(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, bool is_tlb_set);
      ~CacheSetTLBTemplate() {}

      UInt32 getReplacementIndex(CacheCntlr *cntlr);
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      static const int PAGE_2MB = 21;

      UInt8 m_rrpv_max;
      UInt8 m_insert_4k;
      UInt8 m_insert_2m;
      UInt32 m_huge_aging_period;
      UInt32 m_max_huge_ways;

      std::vector<UInt8> m_rrpv;
      std::vector<UInt8> m_age_credit;
      // The victim is chosen before CacheSet::insert() copies in the new block,
      // so its page size is unknown here; remember the way and set its
      // insertion RRPV on the next call into this set.
      SInt32 m_pending_way;

      bool isHuge(UInt32 way) const;
      bool protectionActive() const;
      void resolvePendingInsert();
      void age(bool protect);
};

#endif /* CACHE_SET_TLB_TEMPLATE_H */
