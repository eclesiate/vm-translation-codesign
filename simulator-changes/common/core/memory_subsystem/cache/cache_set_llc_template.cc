#include "cache_set_llc_template.h"
#include "pte_priority_hint.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "log.h"
#include <algorithm>

// CompArch 2025 Lab 6, Task 1.3: PTE-aware RRIP for the LLC (see header).

bool CacheSetLLCTemplate::s_stats_registered = false;
UInt64 CacheSetLLCTemplate::s_fills_pte = 0;
UInt64 CacheSetLLCTemplate::s_fills_pte_hint = 0;
UInt64 CacheSetLLCTemplate::s_fills_data = 0;
UInt64 CacheSetLLCTemplate::s_evict_pte = 0;
UInt64 CacheSetLLCTemplate::s_evict_data = 0;
UInt64 CacheSetLLCTemplate::s_hits_pte = 0;
UInt64 CacheSetLLCTemplate::s_hits_data = 0;
UInt64 CacheSetLLCTemplate::s_cap_reached = 0;

namespace
{
   SInt64 cfgInt(const String &key, core_id_t core_id, SInt64 dflt)
   {
      if (Sim()->getCfg()->hasKey(key))
         return Sim()->getCfg()->getIntArray(key, core_id);
      return dflt;
   }
}

CacheSetLLCTemplate::CacheSetLLCTemplate(
      String cfgname, core_id_t core_id,
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, bool is_tlb_set)
   : CacheSet(cache_type, associativity, blocksize, is_tlb_set)
   , m_hand(0)
   , m_pending_way(-1)
   , m_pending_hint(false)
{
   const String p = cfgname + "/llc_pte/";
   const UInt32 bits = cfgInt(p + "bits", core_id, 2);
   LOG_ASSERT_ERROR(bits >= 1 && bits <= 7, "llc_pte/bits must be in [1,7]");
   m_max_age = (1 << bits) - 1;
   auto clampAge = [this](SInt64 v) { return (UInt8)std::max<SInt64>(0, std::min<SInt64>(v, m_max_age)); };
   m_insert_data = clampAge(cfgInt(p + "insert_data", core_id, m_max_age - 1));
   m_insert_pte = clampAge(cfgInt(p + "insert_pte", core_id, m_max_age >= 2 ? m_max_age - 2 : 0));
   m_insert_pte_hint = clampAge(cfgInt(p + "insert_pte_hint", core_id, 0));
   m_pte_hit_age = clampAge(cfgInt(p + "pte_hit_age", core_id, 0));
   m_max_pte_ways = std::min<SInt64>(cfgInt(p + "max_pte_ways", core_id, associativity / 2), associativity);

   m_age.assign(m_associativity, m_max_age);

   if (!s_stats_registered)
   {
      s_stats_registered = true;
      registerStatsMetric("llc_pte", 0, "fills_pte", &s_fills_pte);
      registerStatsMetric("llc_pte", 0, "fills_pte_hint", &s_fills_pte_hint);
      registerStatsMetric("llc_pte", 0, "fills_data", &s_fills_data);
      registerStatsMetric("llc_pte", 0, "evictions_pte", &s_evict_pte);
      registerStatsMetric("llc_pte", 0, "evictions_data", &s_evict_data);
      registerStatsMetric("llc_pte", 0, "hits_pte", &s_hits_pte);
      registerStatsMetric("llc_pte", 0, "hits_data", &s_hits_data);
      registerStatsMetric("llc_pte", 0, "cap_reached", &s_cap_reached);
      PtePriorityHint::init();
   }
}

bool
CacheSetLLCTemplate::isPTE(UInt32 way) const
{
   if (!m_cache_block_info_array[way]->isValid())
      return false;
   const CacheBlockInfo::block_type_t t = m_cache_block_info_array[way]->getBlockType();
   return t == CacheBlockInfo::block_type_t::PAGE_TABLE
       || t == CacheBlockInfo::block_type_t::PAGE_TABLE_PASSTHROUGH;
}

UInt32
CacheSetLLCTemplate::countPTE() const
{
   UInt32 n = 0;
   for (UInt32 i = 0; i < m_associativity; i++)
      if (isPTE(i))
         n++;
   return n;
}

void
CacheSetLLCTemplate::resolvePendingInsert()
{
   if (m_pending_way < 0)
      return;
   const UInt32 w = m_pending_way;
   m_pending_way = -1;
   if (!m_cache_block_info_array[w]->isValid())
      return;

   if (!isPTE(w))
   {
      m_age[w] = m_insert_data;
      s_fills_data++;
      return;
   }
   s_fills_pte++;
   if (m_pending_hint)
      s_fills_pte_hint++;

   // The new line is already counted in countPTE(); compare against the cap
   // excluding it.
   if (countPTE() > m_max_pte_ways)
   {
      m_age[w] = m_insert_data;        // over the cap: no protection
      s_cap_reached++;
   }
   else
      m_age[w] = m_pending_hint ? m_insert_pte_hint : m_insert_pte;
}

UInt32
CacheSetLLCTemplate::getReplacementIndex(CacheCntlr *cntlr)
{
   resolvePendingInsert();
   m_pending_hint = PtePriorityHint::insertHint();   // set by NucaCache::write for this exact line

   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         m_pending_way = i;
         return i;
      }
   }

   const bool protect = countPTE() < m_max_pte_ways;

   for (UInt32 round = 0; round <= m_max_age; ++round)
   {
      SInt32 victim_data = -1, victim_pte = -1;
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         const UInt32 idx = (m_hand + i) % m_associativity;
         if (m_age[idx] < m_max_age || !isValidReplacement(idx))
            continue;
         if (isPTE(idx))
         {
            if (victim_pte < 0) victim_pte = idx;
         }
         else if (victim_data < 0)
            victim_data = idx;
      }

      // Protected: evict data first. Over the cap: evict PTE lines first so the
      // set drifts back under max_pte_ways.
      SInt32 victim;
      if (protect)
         victim = victim_data >= 0 ? victim_data : victim_pte;
      else
         victim = victim_pte >= 0 ? victim_pte : victim_data;

      if (victim >= 0)
      {
         if (isPTE(victim)) s_evict_pte++; else s_evict_data++;
         m_hand = (victim + 1) % m_associativity;
         m_pending_way = victim;
         return victim;
      }

      for (UInt32 i = 0; i < m_associativity; i++)
         m_age[i] = std::min<UInt8>(m_age[i] + 1, m_max_age);
   }

   // Every line is at max age after (max+1) rounds, so this is only reached if
   // isValidReplacement() rejected all of them.
   for (UInt32 i = 0; i < m_associativity; i++)
      if (isValidReplacement(i))
      {
         m_pending_way = i;
         return i;
      }
   LOG_PRINT_ERROR("LLC template failed to find a victim");
   return 0;
}

void
CacheSetLLCTemplate::updateReplacementIndex(UInt32 accessed_index)
{
   resolvePendingInsert();
   if (isPTE(accessed_index))
   {
      s_hits_pte++;
      m_age[accessed_index] = std::min(m_age[accessed_index], m_pte_hit_age);
   }
   else
   {
      s_hits_data++;
      if (m_age[accessed_index] > 0)
         m_age[accessed_index]--;
   }
}
