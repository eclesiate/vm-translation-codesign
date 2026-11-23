#include "pagetable_aware_allocator.h"
#include "physical_memory_allocator.h"
#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include "utils.h"
#include "log.h"

#include <utility>
#include <cmath>
#include <iostream>
#include <fstream>
#include <list>
#include <vector>

#include <algorithm>
#include <cmath>
#include <random>
#include <tuple>
#include <cassert>

//#define DEBUG_PAGE_TABLE_AWARE_ALLOCATOR

using namespace std;

/*
 * 
 * PageTableAwareAllocator
 * =======================
 *
 * This allocator is used in the lab as the "OS physical memory allocator".
 * It already implements a reservation-based scheme to obtain 2MB regions
 * (for Transparent Huge Pages, THP), and falls back to 4KB allocations via
 * an internal Buddy allocator when 2MB reservations are not possible.
 *
 *   - 4KB pages  : "normal" small pages
 *   - 2MB pages  : large pages (THP)
 *
 * In the baseline code, page tables are *always* allocated as 4KB pages from
 * the generic Buddy allocator, and data (user allocations) may use 2MB chunks.
 *
 * -------------------------------------------------------------------------
 * How this connects to the lab:
 * -------------------------------------------------------------------------
 *  - Task 1.x: Students mainly interact with THP behavior (promotion /
 *              demotion), TLB impact, etc.
 *
 *  - Task 2.2 (DRAM-Aware Page Table Allocation) :
 *      You need to do the following:
 *        * Separate physical frames used for Page Tables from those used for
 *          Application Data, based on the DRAM bank/row mapping.
 *        * Conceptually: "Reserved Region" (for PT pages) vs "Standard Region"
 *          (for data pages).
 *
 *      This allocator is where the OS decides:
 *        - is_pagetable_allocation == true → Page Table page
 *        - is_pagetable_allocation == false → Application Data page
 *
 *      So this file is a *natural hook point* for Task 2.2:
 *        - The branch for is_pagetable_allocation is where you should enforce
 *          "allocate from Reserved DRAM region".
 *        - The else branch is where you should enforce "allocate from
 *          Standard DRAM region".
 *meg
 *      The *implementation details* of "Reserved vs Standard Region" (e.g.,
 *      using different free lists, or manipulating bank bits) will typically
 *      be implemented by extending the Buddy allocator or by adding a small
 *      mapping layer on top of it.
 *
 * -------------------------------------------------------------------------
 * Important data structures already present:
 * -------------------------------------------------------------------------
 *   - two_mb_map:
 *       key   : region_2MB (VA-anchored index, address >> 21)
 *       value : tuple<
 *                   UInt64        region_begin_physical_page_index,
 *                   bitset<512>   per-4KB usage inside this 2MB region,
 *                   bool          promoted (true iff region is now a 2MB THP)
 *                 >
 *
 *   - buddy_allocator:
 *       Fallback allocator for 4KB pages and for reserving 2MB regions
 *       (reserve_2mb_page()).
 *
 *   - stats: counters used in the lab's sim.stats.
 */

PageTableAwareAllocator::PageTableAwareAllocator(String name,
                                                 int memory_size,
                                                 int max_order,
                                                 int kernel_size,
                                                 String frag_type,
                                                 float _threshold_for_promotion)
   : PhysicalMemoryAllocator(name, memory_size, kernel_size),
     threshold_for_promotion(_threshold_for_promotion)
{
   // Log file for debugging and to help students understand allocator behavior.
   log_file_name = "pagetable_aware_allocator.log";
   log_file_name =
      std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
   log_file.open(log_file_name);

   log_file << "[MimicOS] Creating Page Table Aware Allocator" << std::endl;

   // Initialize statistics
   stats.four_kb_allocated = 0;
   stats.two_mb_reserved   = 0;
   stats.two_mb_promoted   = 0;
   stats.two_mb_demoted    = 0;
   stats.total_allocations = 0;
   stats.kernel_pages_used = 0;

   // Underlying Buddy allocator. In the baseline, it manages *all* physical
   // frames, without distinguishing DRAM banks / reserved vs standard regions.
   //
   // === Task 2.2 Hint ===
   // For DRAM-aware page table placement, students can:
   //   - extend Buddy with bank-aware allocation routines, OR
   //   - create additional data structures that track which physical frames
   //     belong to the "Reserved Region" (for page tables) vs "Standard Region".
   //
   buddy_allocator = new Buddy(memory_size, max_order, kernel_size, frag_type);

   // Register stats metrics for the simulator
   registerStatsMetric(name, 0, "four_kb_allocated", &stats.four_kb_allocated);
   registerStatsMetric(name, 0, "two_mb_reserved", &stats.two_mb_reserved);
   registerStatsMetric(name, 0, "two_mb_promoted", &stats.two_mb_promoted);
   registerStatsMetric(name, 0, "two_mb_demoted", &stats.two_mb_demoted);
   registerStatsMetric(name, 0, "total_allocations", &stats.total_allocations);
   registerStatsMetric(name, 0, "kernel_pages_used", &stats.kernel_pages_used);

   // ---- Lab 6 Task 2.2: DRAM-aware page-table placement ----------------
   // Physical address -> DRAM mapping (dram_perf_model_detailed.cc, open-page
   // mapping, column_offset = 0, one controller => linear address == PA):
   //   PA[5:0] line offset | [ch] channel | [col] column | [rk] rank | [bk] bank | rest: row
   // so the bank field starts at PA bit 6 + ch + col + rk (= 17 for ddr4_2400.cfg),
   // i.e. PFN bit 5: every 4KB frame lives in exactly one bank (of one rank).
   stats.pt_frames = stats.pt_frames_in_pt_bank = 0;
   stats.data_frames_4k = stats.data_frames_in_pt_bank = 0;
   stats.pt_pool_frames = stats.data_pool_frames = stats.pt_fallbacks = 0;
   {
      config::Config *cfg = Sim()->getCfg();
      const String p = "perf_model/" + name + "/";
      m_pt_isolation = cfg->hasKey(p + "pt_isolation") ? cfg->getBool(p + "pt_isolation") : false;
      m_num_banks = cfg->getInt("perf_model/dram/ddr/num_banks");
      m_pt_bank = cfg->hasKey(p + "pt_bank") ? cfg->getInt(p + "pt_bank") : m_num_banks - 1;
      m_pt_num_banks = cfg->hasKey(p + "pt_num_banks") ? cfg->getInt(p + "pt_num_banks") : 1;
      // Ablation modes: "full" (default) = PT only in reserved banks AND data never there;
      // "pt_only" = PT confined, data unrestricted; "data_only" = data kept out, PT unrestricted.
      const String mode = cfg->hasKey(p + "pt_isolation_mode") ? cfg->getString(p + "pt_isolation_mode") : "full";
      LOG_ASSERT_ERROR(mode == "full" || mode == "pt_only" || mode == "data_only", "pt_isolation_mode must be full|pt_only|data_only");
      m_isolate_pt = (mode != "data_only");
      m_isolate_data = (mode != "pt_only");
      LOG_ASSERT_ERROR(m_pt_num_banks >= 1 && m_pt_num_banks <= m_pt_bank + 1 && m_pt_num_banks < m_num_banks, "invalid pt_num_banks");
      const UInt32 bank_shift_pa = 6 + cfg->getInt("perf_model/dram/ddr/required_channel_bits")
                                     + cfg->getInt("perf_model/dram/ddr/dram_page_size_bits")
                                     + cfg->getInt("perf_model/dram/ddr/required_rank_bits");
      LOG_ASSERT_ERROR(bank_shift_pa >= 12, "bank bits below the 4KB page offset: bank isolation needs bank bits in the PFN");
      LOG_ASSERT_ERROR((m_num_banks & (m_num_banks - 1)) == 0 && m_pt_bank < m_num_banks, "invalid num_banks/pt_bank");
      if (m_pt_isolation)
      {
         LOG_ASSERT_ERROR(cfg->getBool("perf_model/dram/ddr/open_page_mapping") && cfg->getInt("perf_model/dram/ddr/column_offset") == 0
                          && cfg->getInt("perf_model/dram/num_controllers") == 1,
                          "pt_isolation assumes the open-page mapping with column_offset=0 and a single DRAM controller");
      }
      m_bank_shift_pfn = bank_shift_pa - 12;
      log_file << "[PT-ISOLATION] enabled=" << m_pt_isolation << " mode=" << mode << " pt_banks=" << (m_pt_bank - m_pt_num_banks + 1) << ".." << m_pt_bank
               << " num_banks=" << m_num_banks << " bank bits at PA[" << bank_shift_pa << ".."
               << (bank_shift_pa + floorLog2(m_num_banks) - 1) << "]" << std::endl;
   }
   registerStatsMetric(name, 0, "pt_frames", &stats.pt_frames);
   registerStatsMetric(name, 0, "pt_frames_in_pt_bank", &stats.pt_frames_in_pt_bank);
   registerStatsMetric(name, 0, "data_frames_4k", &stats.data_frames_4k);
   registerStatsMetric(name, 0, "data_frames_in_pt_bank", &stats.data_frames_in_pt_bank);
   registerStatsMetric(name, 0, "pt_pool_frames", &stats.pt_pool_frames);
   registerStatsMetric(name, 0, "data_pool_frames", &stats.data_pool_frames);
   registerStatsMetric(name, 0, "pt_fallbacks", &stats.pt_fallbacks);
}

/*
 * Lab 6 Task 2.2: reserved-region (page-table) frame allocation.
 * Reserved-bank frames encountered while allocating data are parked in
 * m_pt_pool; if it is empty we pull frames from the buddy until one lands in
 * the reserved bank, parking the others in m_data_pool for later data use.
 */
UInt64 PageTableAwareAllocator::allocatePTFrame(UInt64 address, UInt64 core_id)
{
   UInt64 frame = static_cast<UInt64>(-1);
   if (!m_pt_pool.empty())
   {
      frame = m_pt_pool.back();
      m_pt_pool.pop_back();
   }
   else
   {
      while (true)
      {
         UInt64 f = buddy_allocator->allocate(4096, address, core_id);
         if (f == static_cast<UInt64>(-1))
            break;
         if (inReserved(f)) { frame = f; break; }
         m_data_pool.push_back(f);
      }
      if (frame == static_cast<UInt64>(-1) && !m_data_pool.empty())
      {
         // Reserved region exhausted: fall back to a standard frame rather than crash.
         frame = m_data_pool.back();
         m_data_pool.pop_back();
         stats.pt_fallbacks++;
      }
   }
   stats.pt_pool_frames = m_pt_pool.size();
   stats.data_pool_frames = m_data_pool.size();
   return frame;
}

/*
 * Lab 6 Task 2.2: standard-region (data) 4KB frame allocation. Never returns a
 * reserved-bank frame while isolation is on (those are parked for page tables).
 */
UInt64 PageTableAwareAllocator::allocateDataFrame(UInt64 size, UInt64 address, UInt64 core_id)
{
   if (!m_pt_isolation || size != 4096)
      return buddy_allocator->allocate(size, address, core_id);

   UInt64 frame = static_cast<UInt64>(-1);
   if (!m_data_pool.empty())
   {
      // standard-region frames parked by allocatePTFrame() are used first (any mode)
      frame = m_data_pool.back();
      m_data_pool.pop_back();
   }
   else if (!m_isolate_data)
   {
      frame = buddy_allocator->allocate(size, address, core_id);   // "pt_only": data unrestricted
   }
   else
   {
      while (true)
      {
         UInt64 f = buddy_allocator->allocate(4096, address, core_id);
         if (f == static_cast<UInt64>(-1))
            break;
         if (!inReserved(f)) { frame = f; break; }
         m_pt_pool.push_back(f);
      }
   }
   stats.pt_pool_frames = m_pt_pool.size();
   stats.data_pool_frames = m_data_pool.size();
   return frame;
}

PageTableAwareAllocator::~PageTableAwareAllocator()
{
   delete buddy_allocator;
}

/*
 * demote_page()
 * -------------
 * THP-specific helper, used when memory is tight.
 *
 *  - We scan all 2MB regions currently managed in two_mb_map.
 *  - For regions that are *not promoted* (not a full 2MB THP), we compute
 *    their utilization = (#allocated 4KB pages) / 512.
 *  - We select the region with the *lowest* utilization and "demote" it:
 *      * All *unused* 4KB sub-pages of that region are freed back to the
 *        Buddy allocator.
 *      * The 2MB region is removed from two_mb_map.
 *
 * This helps reclaim fragmented space inside partially used 2MB reservations.
 * Returns:
 *   - true  → a region was demoted and some space freed
 *   - false → no suitable region exists (nothing to demote)
 */
bool PageTableAwareAllocator::demote_page()
{
   // Collect <region_2MB_index, utilization> pairs
   std::vector<std::pair<UInt64, double>> utilization;

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Debug: Starting to sort region_2MB based on utilization" << std::endl;
#endif

   for (auto it = two_mb_map.begin(); it != two_mb_map.end(); it++)
   {
      // Skip already promoted regions (these represent full 2MB THPs)
      if (std::get<2>(it->second))
         continue;

      double util = static_cast<double>(std::get<1>(it->second).count()) / 512.0;
      utilization.push_back(std::make_pair(it->first, util));
   }

   // Sort regions by ascending utilization
   std::sort(utilization.begin(), utilization.end(),
             [](std::pair<UInt64, double> &left, std::pair<UInt64, double> &right)
             { return left.second < right.second; });

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Debug: Sorted utilization vector" << std::endl;
#endif

   // No 2MB region to demote
   if (utilization.empty())
   {
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: No regions to demote, returning false" << std::endl;
#endif
      return false;
   }

   // Take the least-utilized region
   UInt64 region_2MB   = utilization[0].first;
   UInt64 region_begin = std::get<0>(two_mb_map[region_2MB]);
   UInt64 region_size  = 512; // 512 * 4KB pages = 2MB
   UInt64 chunk        = region_size;

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Debug: Removing region_2MB with utilization: " << utilization[0].second << std::endl;
#endif

   // Free every unused 4KB sub-page inside this 2MB region back to the Buddy allocator
   for (UInt64 j = 0; j < chunk; j++)
   {
      if (std::get<1>(two_mb_map[region_2MB])[j])
         continue;

      buddy_allocator->free(region_begin + j, region_begin + (j + 1));
   }

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Demoted 2MB region with utilization: " << utilization[0].second
            << " and freed "
            << 512 - std::get<1>(two_mb_map[region_2MB]).count()
            << " pages" << std::endl;
#endif

   stats.two_mb_demoted++;

   // Finally, erase this region from our THP tracking map
   two_mb_map.erase(utilization[0].first);

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Debug: Updated stats and erased region_2MB from two_mb_map" << std::endl;
#endif

   return true;
}

/*
 * checkFor2MBAllocation(...)
 * --------------------------
 * This helper tries to place a given virtual address into a 2MB region (for THP).
 *
 *   - region_2MB = address >> 21  → which 2MB "chunk" in the virtual address space
 *   - If we don't yet have a reservation for that region, we call
 *       buddy_allocator->reserve_2mb_page(address, core_id)
 *     to get a contiguous 2MB block.
 *   - We then track per-4KB usage inside this 2MB block via a bitset<512>.
 *   - Once the fraction of used 4KB pages exceeds threshold_for_promotion,
 *     we "promote" the region to a 2MB page.
 *
 * Returns:
 *   ( physical_base_page_index_for_this_4KB , just_promoted? )
 * or
 *   ( (UInt64)-1 , false ) if 2MB reservation failed.
 */

std::pair<UInt64, bool> PageTableAwareAllocator::checkFor2MBAllocation(UInt64 address, UInt64 core_id)
{
   UInt64 region_2MB = address >> 21;  // which 2MB VA region

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Debug: Calculated region_2MB = " << region_2MB << std::endl;
#endif

   // If we have not yet reserved that 2MB region, try to do so now
   if (two_mb_map.find(region_2MB) == two_mb_map.end())
   {
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: region_2MB not found in two_mb_map" << std::endl;
#endif
      auto two_mb_reserved_region = buddy_allocator->reserve_2mb_page(address, core_id);
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: Called reserve_2mb_page, result = " << std::get<0>(two_mb_reserved_region)
               << std::endl;
#endif
      // If we couldn't reserve 2MB, give up
      if (std::get<0>(two_mb_reserved_region) == static_cast<UInt64>(-1))
      {
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
         log_file << "Debug: two_mb_reserved_region is -1, returning -1" << std::endl;
#endif
         return std::make_pair((UInt64)-1, false);
      }
      else
      {
         // We successfully reserved a 2MB chunk; create a new entry
         two_mb_map[region_2MB] = std::make_tuple(std::get<0>(two_mb_reserved_region),
                                                  std::bitset<512>(),
                                                  false /* not promoted */);
         stats.two_mb_reserved++;
#ifdef DEBUG
         log_file << "Debug: Reserved 2MB region, updated two_mb_map and stats.two_mb_reserved = "
                  << stats.two_mb_reserved << std::endl;
#endif
      }
   }

   // Retrieve region info
   auto &region = two_mb_map[region_2MB];

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Debug: Retrieved region from two_mb_map" << std::endl;
#endif

   // If region is already promoted, we should not be here (no 4KB fault expected)
   if (std::get<2>(region))
   {
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: Page is already promoted" << std::endl;
#endif
      assert(false);
   }
   else
   {
      // Mark the correct 4KB offset within the 2MB region as in use
      auto &bitset = std::get<1>(region);
      int offset_in_2MB = (address >> 12) & 0x1FF;  // 9 bits index [0..511]

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: Page is not promoted, recalculated offset_in_2MB = "
               << offset_in_2MB << std::endl;
#endif

      bitset.set(offset_in_2MB);

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: Marked page as used in bitset: ";
      for (int i = 0; i < 512; i++)
      {
         log_file << bitset[i];
      }
      log_file << std::endl;
#endif

      // Compute utilization for this region
      float utilization = static_cast<float>(bitset.count()) / 512.0f;

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: Calculated utilization = " << utilization
               << " with threshold_for_promotion = "
               << threshold_for_promotion << std::endl;
#endif

      // If utilization exceeds threshold, "promote" region to a 2MB page
      bool ready_to_promote = (utilization > threshold_for_promotion);
      if (ready_to_promote && !std::get<2>(region))
      {
         std::get<2>(region) = true;  // mark as promoted
         stats.two_mb_promoted++;

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
         log_file << "Debug: Promoted page, updated stats.two_mb_promoted = "
                  << stats.two_mb_promoted << std::endl;
#endif
         // Return physical index within region, and mark as just promoted
         return std::make_pair(std::get<0>(region) + offset_in_2MB, true);
      }
      else
      {
         // Still in 4KB mode; return physical index for that 4KB page
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
         log_file << "Debug: Page is not promoted, returning physical address "
                  << std::get<0>(region) + offset_in_2MB  << std::endl;
#endif
         return std::make_pair(std::get<0>(region) + offset_in_2MB, false);
      }
   }

   // Should not reach here
   return std::make_pair((UInt64)-1, false);
}

/**
 * allocate(...)
 * -------------
 * Main entry point for allocations.
 *
 * Parameters:
 *   size                     - requested size (usually page-sized)
 *   address                  - virtual address (used for THP region grouping)
 *   core_id                  - core performing the allocation (for stats/debug)
 *   is_pagetable_allocation  - indicates if this page is for a Page Table
 *                              (true) or for application data (false).
 *
 * Baseline behavior:
 *   - Page Table pages:
 *       → Always 4KB pages via buddy_allocator->allocate()
 *   - Application Data pages:
 *       → Try to place into 2MB region (checkFor2MBAllocation()).
 *       → If 2MB path fails, fall back to 4KB via buddy_allocator.
 *       → If fallback fails, demote a region and try again.
 *
 * Returned pair:
 *   (physical_address, page_size_in_bits)
 *   e.g. (PA, 12) for 4KB pages, (PA, 21) for 2MB pages.
 *
 * -------------------------------------------------------------------------
 * Task 2.2 Hooks (DRAM-aware page table placement) :contentReference[oaicite:2]{index=2}
 * -------------------------------------------------------------------------
 *  - The is_pagetable_allocation flag tells us whether the allocation is
 *    for a Page Table (PT) or for Application Data.
 *
 *  - For Task 2.2, students should:
 *      * Ensure PT pages are allocated from a *Reserved DRAM Region*
 *        (e.g., last DRAM bank).
 *      * Ensure Data pages are allocated from the *Standard Region*
 *        (other banks).
 *
 *  - The simplest conceptual pattern:
 *
 *      if (is_pagetable_allocation)
 *          allocate_from_reserved_region(...);
 *      else
 *          allocate_from_standard_region(...);
 *
 *    where both helpers would internally manipulate physical addresses /
 *    bank bits or use different free lists, but still rely on Buddy to
 *    manage free frames.
 */

std::pair<UInt64, UInt64>
PageTableAwareAllocator::allocate(UInt64 size,
                                  UInt64 address,
                                  UInt64 core_id,
                                  bool is_pagetable_allocation)
{
   stats.total_allocations++;

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "PageTableAwareAllocator::allocate(" << size << ", " << address
            << ", " << core_id << ", is_pagetable=" << is_pagetable_allocation
            << ")" << std::endl;
#endif

   // ---------------------------------------------------------------
   // 1. Page Table allocations (is_pagetable_allocation == true)
   // ---------------------------------------------------------------
   //
   // Baseline:
   //   - Simply allocate a 4KB page from the generic Buddy allocator.
   //   - No DRAM bank isolation: page tables and data may end up in the
   //     same banks and rows.
   //
   // Task 2.2:
   //   - This is where students should route PT pages into the *Reserved
   //     Region* of DRAM, e.g., "Bank 7 only".
   //   - They will likely:
   //        * either extend Buddy to support "allocate_in_bank(bank_id, ...)",
   //        * or maintain a bank-aware free-list on top of it.
   //
   if (is_pagetable_allocation)
   {
      // Lab 6 Task 2.2: reserved region when isolation is on, generic buddy otherwise
      auto page = (m_pt_isolation && m_isolate_pt) ? allocatePTFrame(address, core_id)
                                 : buddy_allocator->allocate(size, address, core_id);
      stats.pt_frames++;
      if (page != static_cast<UInt64>(-1) && inReserved(page))
         stats.pt_frames_in_pt_bank++;
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: Pagetable allocation, result = " << page << std::endl;
#endif
      stats.four_kb_allocated++;
      return make_pair(page, 12);   // always 4KB pages for page tables
   }

   // ---------------------------------------------------------------
   // 2. Application Data allocations (is_pagetable_allocation == false)
   // ---------------------------------------------------------------
   //
   // Baseline (THP-focused):
   //   - Try to place this address inside a 2MB region (THP).
   //   - If that fails, fall back to 4KB allocation via Buddy.
   //
   // Task 2.2:
   //   - Data pages should be allocated from the *Standard Region* of DRAM
   //     (e.g., Banks 0-6).
   //   - The THP logic (2MB reservations) can be combined with bank-aware
   //     placement if desired, but for the lab it is enough that page tables
   //     are in a different bank than data.
   //
   auto page_2mb_result = checkFor2MBAllocation(address, core_id);

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "Debug: Checked for 2MB allocation, result = "
            << page_2mb_result.first << std::endl;
#endif

   // Case A: 2MB region succeeded (either newly reserved or existing)
   if (page_2mb_result.first != static_cast<UInt64>(-1))
   {

#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: There is a 2MB allocation (either promoted now or "
                  "reserved now), returning physical address"
               << std::endl;
#endif
      if (page_2mb_result.second)
      {
         // Just got promoted to 2MB (page_size = 21 bits)
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
         log_file << "Debug: The page just got promoted, returning physical "
                     "address with 2MB flag"
                  << std::endl;
#endif
         return make_pair(page_2mb_result.first, 21);
      }
      else
      {
         // Still operating on 4KB granularity inside the 2MB reservation
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
         log_file << "Debug: The page is not promoted, returning physical "
                     "address with 4KB flag"
                  << std::endl;
#endif
         return make_pair(page_2mb_result.first, 12);
      }
   }
   else
   {
      // Case B: 2MB reservation path failed; fall back to Buddy allocator
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
      log_file << "Debug: No 2MB allocation, falling back to buddy allocator"
               << std::endl;
#endif
      auto page_fallback = allocateDataFrame(size, address, core_id); // Lab 6 Task 2.2: standard region
      if (page_fallback != static_cast<UInt64>(-1))
      {
         stats.data_frames_4k++;
         if (inReserved(page_fallback))
            stats.data_frames_in_pt_bank++;
      }

      // Buddy succeeded → 4KB allocation
      if (page_fallback != static_cast<UInt64>(-1))
      {
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
         log_file << "Debug: Buddy allocator succeeded, returning physical "
                     "address with 4KB flag"
                  << std::endl;
#endif
         stats.four_kb_allocated++;
         return make_pair(page_fallback, 12);
      }
      else
      {
         // Buddy also failed → try to demote a partially used 2MB region
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
         log_file << "Debug: Buddy allocator failed, attempting to demote a page"
                  << std::endl;
#endif
         bool demoted = demote_page();
         if (demoted)
         {
            stats.four_kb_allocated++;
            auto page_retry = allocateDataFrame(size, address, core_id); // Lab 6 Task 2.2
            return make_pair(page_retry, 12);
         }
         else
         {
            // Out of options → out of memory
            assert(false);
            return make_pair((UInt64)-1, 12);
         }
      }
   }
}

/*
 * givePageFast(...)
 * -----------------
 * Fast path that bypasses THP logic and uses a direct Buddy allocation.
 *
 * Used when THP is not needed or not applicable. It always returns 4KB pages.
 */
UInt64 PageTableAwareAllocator::givePageFast(UInt64 bytes,
                                             UInt64 address,
                                             UInt64 core_id)
{
#ifdef DEBUG_PAGE_TABLE_AWARE_ALLOCATOR
   log_file << "PageTableAwareAllocator::givePageFast(" << bytes << ", "
            << address << ", " << core_id << ")" << std::endl;
#endif
   return buddy_allocator->allocate(bytes, address, core_id);
}

/*
 * deallocate(...)
 * ---------------
 * Not implemented in the baseline code.
 *
 * If extended, it would need to:
 *   - Distinguish whether the physical page belongs to:
 *       * a 4KB-only region managed by Buddy, OR
 *       * a 2MB reservation (update bitset + possibly demote).
 *   - In a Task 2.2 extension, it could also:
 *       * maintain separate accounting for Reserved vs Standard regions.
 */
void PageTableAwareAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
   /* Requires implementation */
}

/*
 * allocate_ranges(...)
 * --------------------
 * Not used in this model. Could be extended to allocate a continuous VA range.
 */
std::vector<Range> PageTableAwareAllocator::allocate_ranges(IntPtr start_va,
                                                            IntPtr end_va,
                                                            int app_id)
{
   std::vector<Range> ranges;
   return ranges;
}

/*
 * Simple pass-throughs to Buddy allocator for stats.
 */
UInt64 PageTableAwareAllocator::getFreePages() const
{
   return buddy_allocator->getFreePages();
}

UInt64 PageTableAwareAllocator::getTotalPages() const
{
   return buddy_allocator->getTotalPages();
}

double PageTableAwareAllocator::getLargePageRatio()
{
   return buddy_allocator->getLargePageRatio();
}

double PageTableAwareAllocator::getAverageSizeRatio()
{
   return buddy_allocator->getAverageSizeRatio();
}

/*
 * fragment_memory()
 * -----------------
 * Forces fragmentation in the Buddy allocator (used in some experiments).
 */
void PageTableAwareAllocator::fragment_memory()
{
   buddy_allocator->fragmentMemory(
      Sim()->getCfg()->getFloat("perf_model/" + m_name + "/target_fragmentation"));
   return;
}
