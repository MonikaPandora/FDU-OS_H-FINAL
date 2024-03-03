#include "aarch64/intrinsic.h"
#include "kernel/printk.h"
#include <aarch64/mmu.h>
#include <common/checker.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/init.h>
#include <kernel/mem.h>

#define K_DEBUG 0

#define FAIL(...)                                                              \
    {                                                                          \
        printk(__VA_ARGS__);                                                   \
        while (1)                                                              \
            ;                                                                  \
    }

RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt) { init_rc(&alloc_page_cnt); }

typedef struct block_header_t
{
    // 一般情况下next是下一个块的内核虚拟地址的值
    // 如果next是一个page的头地址，表示该block属于该page，并且被申请使用
    u64 next;
}block_header_t;

typedef struct page_header_t
{
    // 用于链接的链表节点
    ListNode node;
    // 该page中可分配块的空闲链表的头指针
    block_header_t* free_blocks_head;
    // 该page中的块可分配大小（即除开block_header_t的长度）均为8 * key
    i32 key;
    // 记录该page中有多少个块被申请后还未被释放
    u32 ref;
}page_header_t;

#define MAX_BLOCK_SIZE (PAGE_SIZE - sizeof(page_header_t) - sizeof(block_header_t))
#define PAGE_ALLOC_LEN (1 + MAX_BLOCK_SIZE / 8)
#define MAX_KEY (MAX_BLOCK_SIZE / 8)
static QueueNode* pages;
static ListNode* page_alloc[PAGE_ALLOC_LEN];

static SpinLock kmem_lock;

define_early_init(kmem_lock)
{
    init_spinlock(&kmem_lock);
}

extern char end[];
static void* zero_page;
struct page _pages[ALL_PAGE_COUNT];
define_early_init(pages)
{
    zero_page = (void*)(PAGE_BASE((u64)&end) + PAGE_SIZE);
    for(int i = 0; i < PAGE_SIZE; i++){
        ((u8*)zero_page)[i] = 0;
    }
    for (u64 p = ((u64)zero_page + PAGE_SIZE); p < P2K(PHYSTOP); p += PAGE_SIZE)
	   add_to_queue(&pages, (QueueNode*)p);
}

void* kalloc_page()
{
    _increment_rc(&alloc_page_cnt);
    // TODO
    void* page = NULL;
    if(pages) page = fetch_from_queue(&pages);
    if(page) *(u64*)page = 0;
    ASSERT(_pages[PAGE_INDEX(page)].ref.count == 0);
    _increment_rc(&_pages[PAGE_INDEX(page)].ref);
    return page;
}

void kshare_page(u64 addr){
    auto index = PAGE_INDEX(PAGE_BASE(addr));
    _increment_rc(&_pages[index].ref);
}

usize get_page_ref(u64 addr){
    auto index = PAGE_INDEX(PAGE_BASE(addr));
    _acquire_spinlock(&kmem_lock);
    auto ret = _pages[index].ref.count;
    _release_spinlock(&kmem_lock);
    return ret;
}

#define IS_PAGE_ADDR(addr) (PAGE_BASE(addr) == (addr)) 

void kfree_page(void* p)
{
    if(p == get_zero_page())return;
    // TODO
    if(!IS_PAGE_ADDR((u64)p)){
        printk("kfree: wrong page to be freed\n");
        PANIC();
    }
    auto index = PAGE_INDEX(p);
    if(_decrement_rc(&_pages[index].ref)){
        _decrement_rc(&alloc_page_cnt);
        if(_pages[index].ref.count != 0){
            printk("page:%p count: %llu\n", p, _pages[index].ref.count);
            PANIC();
        }
        add_to_queue(&pages, (QueueNode*)p);
    }
}

u64 left_page_cnt() { 
    return ALLOCATABLE_PAGE_COUNT - alloc_page_cnt.count;
}

WARN_RESULT void *get_zero_page() {
    // TODO
    // Return the shared zero page
    return zero_page;
}

// TODO: kalloc kfree
void div_page(page_header_t* page_header, isize block_size){
    block_header_t* first = page_header->free_blocks_head;
    isize remain = PAGE_SIZE - sizeof(page_header_t);
    isize real_block_size = block_size + sizeof(block_header_t);
    first = (block_header_t*)((u64)page_header + sizeof(page_header_t));
    page_header->free_blocks_head = first;
    while(remain >= real_block_size){
        remain -= real_block_size;
        if(remain < real_block_size)first->next = NULL;
        else {
            first->next = (u64)first + real_block_size;
            first = (block_header_t*)first->next;
        }
    }
}

page_header_t* init_page_header(void* page){
    page_header_t* page_header;
    block_header_t* block_header;
    if(page == NULL)PANIC();
    page_header = (page_header_t*)page;
    block_header = (block_header_t*)((u64)page_header + sizeof(page_header_t));
    page_header->free_blocks_head = block_header;
    block_header->next = NULL;
    page_header->key = (page_header->ref = 0);
    init_list_node(&page_header->node);
    return page_header;
}

void* kalloc(isize size){
    void* alloc_ptr = NULL;
    isize key = (size + 7) / 8;
    page_header_t* page = NULL;
    setup_checker(kalloc_checker);
    acquire_spinlock(kalloc_checker, &kmem_lock);
    if(page_alloc[key]){
        _for_in_list(valptr, page_alloc[key]){
            page = (page_header_t*)valptr;
            if(page->free_blocks_head){
                page_alloc[key] = _detach_from_list(valptr);
                break;
            }
            else page = NULL;
        }
    }

    if(!page){
        page = init_page_header(kalloc_page());
        page->key = key;
        div_page(page, 8 * key);
    }

    block_header_t* block = page->free_blocks_head;
    alloc_ptr = (void*)((u64)block + sizeof(block_header_t));
    page->free_blocks_head = (block_header_t*)block->next;
    block->next = (u64)page;
    page->ref++;

    if(page_alloc[key])
        _insert_into_list(page_alloc[key], &page->node);
    page_alloc[key] = &page->node;
    release_spinlock(kalloc_checker, &kmem_lock);
    return alloc_ptr;
}

void kfree(void* p){
    setup_checker(kfree_checker);
    acquire_spinlock(kfree_checker, &kmem_lock);
    block_header_t* block = (block_header_t*)((u64)p - sizeof(block_header_t));
    page_header_t* page = (page_header_t*)(block->next);
    block->next = (u64)page->free_blocks_head;
    page->free_blocks_head = block;
    page->ref--;
    if(page->ref == 0){
        page_alloc[page->key] = _detach_from_list(&page->node);
        kfree_page(page);
    }
    release_spinlock(kfree_checker, &kmem_lock);
}

