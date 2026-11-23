#include "cache_set_tlb_template.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"

// CompArch 2025 Lab 6, Task 1.2: page-size-aware SRRIP (see header for the policy).

namespace
{
   SInt64 cfgInt(const String &key, core_id_t core_id, SInt64 dflt)
   {
      if (Sim()->getCfg()->hasKey(key))
         return Sim()->getCfg()->getIntArray(key, core_id);
      return dflt;
   }
}

CacheSetTLBTemplate::CacheSetTLBTemplate(
      String cfgname, core_id_t core_id,
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, bool is_tlb_set)
   : CacheSet(cache_type, associativity, blocksize, is_tlb_set)
   , m_pending_way(-1)
{
   const String p = cfgname + "/psa/";
   const UInt32 bits = cfgInt(p + "bits", core_id, 2);
   LOG_ASSERT_ERROR(bits >= 1 && bits <= 7, "psa/bits must be in [1,7]");
   m_rrpv_max = (1 << bits) - 1;
   m_insert_4k = std::min<SInt64>(cfgInt(p + "insert_4k", core_id, m_rrpv_max - 1), m_rrpv_max);
   m_insert_2m = std::min<SInt64>(cfgInt(p + "insert_2m", core_id, m_rrpv_max >= 2 ? 1 : 0), m_rrpv_max);
   m_huge_aging_period = std::max<SInt64>(cfgInt(p + "huge_aging_period", core_id, 2), 1);
   m_max_huge_ways = std::min<SInt64>(cfgInt(p + "max_huge_ways", core_id, associativity > 2 ? associativity - 2 : associativity), associativity);

   m_rrpv.assign(m_associativity, m_rrpv_max);
   m_age_credit.assign(m_associativity, 0);
}

bool
CacheSetTLBTemplate::isHuge(UInt32 way) const
{
   return m_cache_block_info_array[way]->isValid()
       && m_cache_block_info_array[way]->getPageSize() == PAGE_2MB;
}

bool
CacheSetTLBTemplate::protectionActive() const
{
   UInt32 huge = 0;
   for (UInt32 i = 0; i < m_associativity; i++)
      if (isHuge(i))
         huge++;
   return huge <= m_max_huge_ways;
}

void
CacheSetTLBTemplate::resolvePendingInsert()
{
   if (m_pending_way < 0)
      return;
   const UInt32 w = m_pending_way;
   m_pending_way = -1;
   if (!m_cache_block_info_array[w]->isValid())
      return;
   m_rrpv[w] = isHuge(w) ? m_insert_2m : m_insert_4k;
   m_age_credit[w] = 0;
}

void
CacheSetTLBTemplate::age(bool protect)
{
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (m_rrpv[i] >= m_rrpv_max)
         continue;
      if (protect && isHuge(i))
      {
         if (++m_age_credit[i] < m_huge_aging_period)
            continue;
         m_age_credit[i] = 0;
      }
      m_rrpv[i]++;
   }
}

UInt32
CacheSetTLBTemplate::getReplacementIndex(CacheCntlr *cntlr)
{
   resolvePendingInsert();

   // Cold start: always take an invalid way first.
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         m_pending_way = i;
         return i;
      }
   }

   const bool protect = protectionActive();

   // Bounded: every round ages each 4KB entry (and, every huge_aging_period
   // rounds, each 2MB entry) by one, so some way reaches max within
   // (max+1) * huge_aging_period rounds.
   const UInt32 max_rounds = (m_rrpv_max + 1) * m_huge_aging_period + 1;
   for (UInt32 round = 0; round <= max_rounds; round++)
   {
      SInt32 victim_4k = -1, victim_2m = -1;
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_rrpv[i] < m_rrpv_max || !isValidReplacement(i))
            continue;
         if (isHuge(i))
         {
            if (victim_2m < 0) victim_2m = i;
         }
         else if (victim_4k < 0)
            victim_4k = i;
      }

      SInt32 victim = -1;
      if (victim_4k >= 0 && (protect || victim_2m < 0))
         victim = victim_4k;
      else if (victim_2m >= 0 && (!protect || victim_4k < 0))
         victim = victim_2m;
      else if (victim_4k >= 0)
         victim = victim_4k;

      if (victim >= 0)
      {
         m_pending_way = victim;
         return victim;
      }
      age(protect);
   }

   // Fallback (e.g. every way locked by isValidReplacement): first valid candidate.
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (isValidReplacement(i))
      {
         m_pending_way = i;
         return i;
      }
   }
   LOG_PRINT_ERROR("No valid TLB victim found");
   return 0;
}

void
CacheSetTLBTemplate::updateReplacementIndex(UInt32 accessed_index)
{
   resolvePendingInsert();
   m_rrpv[accessed_index] = 0;   // hit priority: predicted near re-reference
   m_age_credit[accessed_index] = 0;
}
