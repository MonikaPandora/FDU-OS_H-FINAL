#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <fs/file.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>


define_rest_init(paging) {
    // TODO
}

void init_sections(ListNode *section_head) {
    // TODO
    struct section* heap = (struct section*)kalloc(sizeof(struct section));
    memset(heap, 0, sizeof(struct section));
    heap->begin = heap->end = 0;
    heap->flags = ST_HEAP;
    _insert_into_list(section_head, &heap->stnode);
}

void free_section_pages(struct pgdir* pd, struct section* sec){
    for(auto i = PAGE_BASE(sec->begin); i < sec->end; i+= PAGE_SIZE){
        auto pte = get_pte(pd, i, false);
        if(pte && (*pte & PTE_VALID)){
            if((sec->flags == ST_MMAP_PRIVATE || sec->flags == ST_MMAP_SHARED) 
                && !(*pte & PTE_RO) && get_page_ref(P2K(PTE_ADDRESS(*pte))) == 1){
                if(sec->fp->type == FD_INODE){
                    u64 this_begin = MAX(i, sec->begin);
                    u64 this_end = MIN(i + PAGE_SIZE, sec->end);
                    sec->fp->off = sec->offset + this_begin - sec->begin;
                    file_write(sec->fp, (char*)P2K(PTE_ADDRESS(*pte)), this_end - this_begin);
                }
                else if(sec->fp->type == FD_PIPE){
                    // TODO
                    PANIC();
                }
                else PANIC();
            }
            kfree_page((void*)P2K(PTE_ADDRESS(*pte)));
            *pte = NULL;
        }
    }
}

void free_sections(struct pgdir *pd) {
    // TODO
    setup_checker(0);
    acquire_spinlock(0, &pd->lock);
    auto p = pd->section_head.next;
    while(p){
        if(p != &pd->section_head){
            auto sec = container_of(p, struct section, stnode);
            free_section_pages(pd, sec);
            p = p->next;
            _detach_from_list(&sec->stnode);
            if(sec->fp)file_close(sec->fp);
            kfree(sec);
        }
        else break;
    }
    release_spinlock(0, &pd->lock);
}

u64 sbrk(i64 size) {
    // TODO:
    // Increase the heap size of current process by `size`
    // If `size` is negative, decrease heap size
    // `size` must be a multiple of PAGE_SIZE
    // Return the previous heap_end
    if(size > 0 )ASSERT(size % PAGE_SIZE == 0);
    else ASSERT((~size+1) % PAGE_SIZE == 0);

    auto this = thisproc();
    auto pd = &this->pgdir;
    u64 ret = 0;
    bool heap_exist = false;
    setup_checker(0);
    acquire_spinlock(0, &pd->lock);
    _for_in_list(p, &pd->section_head){
        if(p == &pd->section_head)continue;
        auto sec = container_of(p, struct section, stnode);
        if(sec->flags == ST_HEAP){
            heap_exist = true;
            if(size == 0){
                release_spinlock(0, &pd->lock);
                return sec->end;
            }
            ret = sec->end;
            sec->end += size;
            if(size > 0)ASSERT(sec->end > ret);
            else if(sec->end > ret)sec->end = 0;

            for(auto i = sec->end; i < ret; i += PAGE_SIZE){
                auto pte = get_pte(pd, i, false);
                if(pte && *pte & PTE_VALID){
                    kfree_page((void*)P2K(PTE_ADDRESS(*pte)));
                    *pte = 0;
                }
            }

            break;
        }
    }
    release_spinlock(0, &pd->lock);
    ASSERT(heap_exist);
    return ret;
}

#define ISS_TYPE_MASK 0x3c
#define ISS_TRANS_FAULT 0X4
#define ISS_ACC_FAULT 0X8
#define ISS_PERMI_FAULT 0Xc

int mmap_handler(struct section* sec, u64 iss, u64 addr){
    struct pgdir *pd = &thisproc()->pgdir;
    if((ISS_TYPE_MASK & iss) == ISS_PERMI_FAULT){
        // try to write on PTE_RO
        auto pte = get_pte(pd, addr, false);
        ASSERT(pte);

        if(!(sec->prot&PROT_WRITE)){
            // illegal
            exit(-1);
        }
        else{
            if(sec->flags == ST_MMAP_PRIVATE && get_file_ref(sec->fp) > 1){
                // other process are using
                // copy to new page
                auto pg = kalloc_page();
                memcpy(pg, (void*)P2K(PTE_ADDRESS(*pte)), PAGE_SIZE);
                kfree_page((void*)P2K(PTE_ADDRESS(*pte)));
                *pte = K2P(pg) | PTE_USER_DATA | PTE_RW;
            }
            else{
                *pte = P2K(PTE_ADDRESS(*pte)) | PTE_USER_DATA | PTE_RW;
            }
        }
    }
    else if(((ISS_TYPE_MASK & iss) == ISS_TRANS_FAULT)){
        // file unload
        auto this_begin = MAX(PAGE_BASE(addr), sec->begin);
        auto this_end = MIN((PAGE_BASE(addr) + PAGE_SIZE), sec->end);
        auto pg = kalloc_page();
        memset(pg, 0, PAGE_SIZE);
        sec->fp->off = sec->offset + this_begin - sec->begin;
        file_read(sec->fp, (char*)pg, this_end - this_begin);
        auto pte = get_pte(pd, addr, true);
        *pte = K2P(pg) | PTE_USER_DATA | PTE_RO;
    }
    else{
        exit(-1);
    }
    return 0;
}

int pgfault_handler(u64 iss) {
    struct proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    u64 addr = arch_get_far(); // Attempting to access this address caused the
                               // page fault
    // TODO:
    // 1. Find the section struct that contains the faulting address `addr`
    // 2. Check section flags to determine page fault type
    // 3. Handle the page fault accordingly
    // 4. Return to user code or kill the process
    struct section* sec = NULL;
    setup_checker(0);
    acquire_spinlock(0, &pd->lock);
    _for_in_list(valptr, &pd->section_head){
        if(valptr == &pd->section_head){
            sec = NULL;
            continue;
        }
        sec = container_of(valptr, struct section, stnode);
        if(sec->begin <= addr && addr < sec->end)break;
    }
    ASSERT(sec);

    if(sec->flags == ST_MMAP_PRIVATE || sec->flags ==ST_MMAP_SHARED){
        int ret =  mmap_handler(sec, iss, addr);
        release_spinlock(0, &pd->lock);
        return ret;
    }
    
    if((ISS_TYPE_MASK & iss) == ISS_PERMI_FAULT){
        // Copy on Write
        // if(sec->flags == ST_HEAP || sec->flags == ST_DATA){
        ASSERT(sec->flags != ST_TEXT);
        auto pg = kalloc_page();
        auto pte = get_pte(pd, addr, false);
        ASSERT(pte);
        memcpy(pg, (void*)P2K(PTE_ADDRESS(*pte)), PAGE_SIZE);
        kfree_page((void*)P2K(PTE_ADDRESS(*pte)));  // unshare the previously shared page
        vmmap(pd, addr, pg, PTE_USER_DATA | PTE_RW);
        // }
        // else if(sec->flags == ST_USER_STACK || sec->flags == ST_TEXT){
        //     // this only happens in a forked children processp
        //     // and its parent process has loaded the corresponding seciton

        // }
    }
    else if(((ISS_TYPE_MASK & iss) == ISS_TRANS_FAULT)){
        //Lazy Allocation
        if(sec->flags == ST_HEAP){
            void* p = kalloc_page();
            vmmap(pd, addr, p, PTE_USER_DATA | PTE_RW);
        }
        else if(sec->flags == ST_TEXT){
            if(sec->length == 0){
                printk("(Error): text section with length 0");
                exit(-1);
            }
            usize len = sec->length;
            u64 va = sec->begin;
            sec->fp->off = sec->offset;
            while(len){
                usize cur_len = MIN(len, PAGE_SIZE - VA_OFFSET(va));
                auto pte = get_pte(pd, va, true);
                if(!(*pte & PTE_VALID)){
                    void* p = kalloc_page();
                    vmmap(pd, va, p, PTE_USER_DATA | PTE_RO);
                }
                if(file_read(sec->fp, (char*)(P2K(PTE_ADDRESS(*pte)) + VA_OFFSET(va)), cur_len) != (isize)cur_len)PANIC();
                len -= cur_len;
                va += cur_len;
            }
            sec->length = 0;
            file_close(sec->fp);
            sec->fp = 0;
        }
        else if(sec->flags == ST_DATA){
            // data and bss section
            // not use lazy allocation
            printk("(Error): did not apply lazy allocation on data and bss section");
            exit(-1);
        }
        else if(sec->flags == ST_USER_STACK){
            printk("(Error): did not apply lazy allocation on user stack");
            exit(-1);
        }
        
    }
    else if((ISS_TYPE_MASK & iss) == ISS_ACC_FAULT){
        PANIC();
    }
    else{
        printk("unknown\n");
        exit(-1);
    }
    release_spinlock(0, &pd->lock);
    arch_tlbi_vmalle1is();
    return 0;
}

void copy_sections(ListNode* from_head, ListNode* to_head){
	_for_in_list(node, from_head){
		if(node == from_head){
			break;
		}
		struct section* st = container_of(node, struct section, stnode);
		struct section* new_st = kalloc(sizeof(struct section));
		memmove(new_st, st, sizeof(struct section));
		if(st->fp != NULL){
			new_st->fp = file_dup(st->fp);
		}
		_insert_into_list(to_head, &(new_st->stnode));
	}
}