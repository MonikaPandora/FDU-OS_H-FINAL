#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/pt.h>
#include <kernel/paging.h>
#include <kernel/printk.h>

static inline PTEntriesPtr alloc_pte(){
    PTEntriesPtr pte = (PTEntriesPtr)kalloc_page();
    if(pte == NULL)PANIC();
    memset(pte, NULL, PAGE_SIZE);
    return pte;
}

#define CHECK_VALID(pte) (((u64)pte & PTE_VALID) == 1)
PTEntriesPtr get_pte(struct pgdir* pgdir, u64 va, bool alloc)
{
    // TODO
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.

    if(pgdir->pt == NULL && alloc == false)return NULL;
    u64 index[] = {VA_PART0(va), VA_PART1(va), VA_PART2(va)};
    if(pgdir->pt == NULL)pgdir->pt = alloc_pte();
    PTEntriesPtr curr = pgdir->pt;
    for(int level = 0; level < 3; level++){
        curr += index[level];
        if(*curr == NULL && alloc == false)return NULL;
        if(*curr == NULL){
            PTEntriesPtr new_entry = alloc_pte();
            *curr = K2P(new_entry) | PTE_TABLE;
        }
        curr = (PTEntriesPtr)P2K(PTE_ADDRESS(*curr));
    }
    return curr + VA_PART3(va);

    // if(!CHECK_VALID(pgdir->pt) && alloc == false)return NULL;
    // u64 index[] = {VA_PART0(va), VA_PART1(va), VA_PART2(va)};
    // if(!CHECK_VALID(pgdir->pt)){
    //     if(pgdir->pt == NULL)pgdir->pt = alloc_pte();
    // }
    // PTEntriesPtr curr = pgdir->pt;
    // for(int level = 0; level < 3; level++){
    //     curr += index[level];
    //     if(!CHECK_VALID(*curr)&& alloc == false)return NULL;
    //     if(!CHECK_VALID(*curr)){
    //         if(*curr == NULL){
    //             PTEntriesPtr new_entry = alloc_pte();
    //             *curr = K2P(new_entry) | PTE_TABLE;
    //         }
    //     }
    //     curr = (PTEntriesPtr)P2K(PTE_ADDRESS(*curr));
    // }
    // return curr + VA_PART3(va);
}

void init_pgdir(struct pgdir *pgdir) { 
    pgdir->pt = alloc_pte();
    ASSERT(pgdir->pt);
    init_spinlock(&pgdir->lock);
    init_list_node(&pgdir->section_head);
    init_sections(&pgdir->section_head);
}

void free_pte(PTEntriesPtr ptb, int level){
    if(level < 2){
        for(int i = 0; i < N_PTE_PER_TABLE; i++){
            if(ptb[i])free_pte((PTEntriesPtr)P2K(PTE_ADDRESS(ptb[i])), level + 1);
        }
    }
    for(int i = 0; i < N_PTE_PER_TABLE; i++){
        if(ptb[i]){
            memset((void*)P2K(PTE_ADDRESS(ptb[i])), NULL, PAGE_SIZE);
            kfree_page((void*)P2K(PTE_ADDRESS(ptb[i])));
        }
    }
}

void free_pgdir(struct pgdir* pgdir)
{
    // TODO
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
    
    free_sections(pgdir);
    if(pgdir->pt){
        free_pte(pgdir->pt, 0);
        memset(pgdir->pt, NULL, PAGE_SIZE);
        kfree_page(pgdir->pt);
    }
}

void attach_pgdir(struct pgdir *pgdir) {
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}

void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags) {
    // TODO
    // Map virtual address 'va' to the physical address represented by kernel
    // address 'ka' in page directory 'pd', 'flags' is the flags for the page
    // table entry
    u64 pa = K2P(ka);
    PTEntriesPtr pte = get_pte(pd, va, true);
    ASSERT(pte);
    *pte = PAGE_BASE(pa) | flags;
    arch_tlbi_vmalle1is();
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len) {
    // TODO
    while (len > 0){
        usize cur_sz = MIN(len, PAGE_SIZE - VA_OFFSET(va));
        auto pte = get_pte(pd, (u64)va, true);
        if(*pte == NULL){
            void* tmp = kalloc_page();
            *pte = K2P(tmp) | PTE_USER_DATA;
        }
        memcpy((void*)(P2K(PTE_ADDRESS(*pte)) + VA_OFFSET(va)), p, cur_sz);
        len -= cur_sz;
        p += cur_sz;
        va += cur_sz;
    }
    return 0;
}