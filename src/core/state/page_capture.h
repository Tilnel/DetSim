#pragma once
#include "page_snapshot.h"

bool page_capture_enabled();
hash_type save_paged_snapshot(pid_t pid, const std::vector<RegionInfo>& regions,
    std::span<const uint8_t> metadata, std::string_view maps,
    const user_regs_struct& regs, uintptr_t rseq_address, int rseq_length,
    hash_type reference_hash);
void reset_page_capture_stats();
void report_page_capture_stats();
