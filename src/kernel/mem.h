#pragma once

#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/rc.h>
#include <driver/memlayout.h>

// #define PAGE_COUNT ((P2K(PHYSTOP) - PAGE_BASE((u64) & end)) / PAGE_SIZE - 1)
#define ALL_PAGE_COUNT ((PHYSTOP - EXTMEM) / PAGE_SIZE)
#define ALLOCATABLE_PAGE_COUNT ((P2K(PHYSTOP) - PAGE_BASE((u64) & end)) / PAGE_SIZE - 1)
#define PAGE_INDEX(page_base) ((KSPACE((u64)page_base) - P2K(EXTMEM)) / PAGE_SIZE)
extern char end[];
struct page {
    RefCount ref;
};

u64 left_page_cnt();

WARN_RESULT void *get_zero_page();

WARN_RESULT void *kalloc_page();
void kfree_page(void *);
void kshare_page(u64);
usize get_page_ref(u64);

WARN_RESULT void *kalloc(isize);
void kfree(void *);