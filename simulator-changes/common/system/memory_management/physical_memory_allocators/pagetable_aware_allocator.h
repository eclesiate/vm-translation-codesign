#pragma once
#include <vector>
#include <map>
#include <bitset>

#include "buddy_allocator.h"
#include "physical_memory_allocator.h"
#include "rangelb.h"

using namespace std;

class PageTableAwareAllocator : public PhysicalMemoryAllocator
{
public:
    PageTableAwareAllocator(String name, int memory_size, int max_order, int kernel_size, String frag_type, float threshold_for_promotion);
    ~PageTableAwareAllocator();
    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false);
    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id);
    UInt64 givePageFast(UInt64 size, UInt64 address = 0, UInt64 core_id = -1);
    void deallocate(UInt64 region, UInt64 core_id);
    void fragment_memory();
    std::pair<UInt64,bool> checkFor2MBAllocation(UInt64 address, UInt64 core_id);
    bool demote_page();

    IntPtr isLargePageReserved(IntPtr address){
        if (two_mb_map.find(address >> 21) != two_mb_map.end())
            return get<0>(two_mb_map[address >> 21]);
        return -1;
    }

    virtual UInt64 handle_page_table_allocations(UInt64 bytes)
    {
        // Lab 6 Task 2.2: with isolation on, kernel-allocated page-table frames
        // (e.g. the radix root) also come from the reserved bank.
        if (m_pt_isolation && m_isolate_pt && bytes == 4096)
            return allocatePTFrame(0, 0);
        stats.kernel_pages_used += 1;
        UInt64 temp = kernel_start_address;
        kernel_start_address += bytes; // This function is useful mainly for the page table allocation in the HDC, HT and Elastic Cuckoo Hash Table; For Radix we can invoke the conventional allocator directly
       // std::cout << "Kernel pages allocated" << stats.kernel_pages_used << std::endl;

        return temp/4096;
    }

    virtual void handle_page_table_deallocations(UInt64 bytes)
    {
        stats.kernel_pages_used -= 1;
        kernel_start_address -= bytes;
        //std::cout << "Kernel pages after deallocation: " << stats.kernel_pages_used << std::endl;

    }

    UInt64 getFreePages() const;
    UInt64 getTotalPages() const;
    double getAverageSizeRatio();
    double getLargePageRatio();

    struct
    {
        UInt64 four_kb_allocated;
        UInt64 two_mb_reserved;
        UInt64 two_mb_promoted;
        UInt64 two_mb_demoted;
        UInt64 total_allocations;
        UInt64 kernel_pages_used;
        // Lab 6 Task 2.2: DRAM bank placement of 4KB frames
        UInt64 pt_frames;               // page-table frames handed out
        UInt64 pt_frames_in_pt_bank;    // ... that landed in the reserved bank
        UInt64 data_frames_4k;          // 4KB data frames handed out (buddy fallback path)
        UInt64 data_frames_in_pt_bank;  // ... that landed in the reserved bank (0 when isolated)
        UInt64 pt_pool_frames;          // reserved-bank frames currently parked for future PTs
        UInt64 data_pool_frames;        // standard-region frames currently parked for data
        UInt64 pt_fallbacks;            // PT allocations that could not get a reserved-bank frame
    } stats;

    double (PageTableAwareAllocator::*frag_fun)();

    std::ofstream log_file;
    std::string log_file_name;

protected:
    Buddy *buddy_allocator;
    // This threshold is used to determine when to promote a 2MB region based on the 4KB page utilization
    float threshold_for_promotion;
    // This map is used to track 2MB-large regions
    std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>> two_mb_map; // <region_2MB, <region_begin, bitset, promoted>>
    UInt64 m_frag_factor;

    // Lab 6 Task 2.2: DRAM-aware page-table placement ("bank isolation").
    // Reserved region = every 4KB frame whose DRAM bank index == m_pt_bank
    // (i.e. that bank in every channel and rank); standard region = all others.
    bool m_pt_isolation;
    bool m_isolate_pt;      // PT frames come only from the reserved banks
    bool m_isolate_data;    // data frames never come from the reserved banks
    UInt32 m_pt_bank;       // highest reserved bank
    UInt32 m_pt_num_banks;  // reserved banks = [m_pt_bank - m_pt_num_banks + 1, m_pt_bank]
    UInt32 m_num_banks;
    UInt32 m_bank_shift_pfn;            // bank bits start at PFN bit m_bank_shift_pfn
    std::vector<UInt64> m_pt_pool;      // free reserved-bank frames (PFNs)
    std::vector<UInt64> m_data_pool;    // free standard-region frames (PFNs)

    UInt32 bankOf(UInt64 pfn) const { return (pfn >> m_bank_shift_pfn) & (m_num_banks - 1); }
    bool inReserved(UInt64 pfn) const { UInt32 b = bankOf(pfn); return b <= m_pt_bank && b + m_pt_num_banks > m_pt_bank; }
    UInt64 allocatePTFrame(UInt64 address, UInt64 core_id);
    UInt64 allocateDataFrame(UInt64 size, UInt64 address, UInt64 core_id);

};
