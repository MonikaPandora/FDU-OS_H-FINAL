#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block device and super block to use.
            Correspondingly, you should NEVER use global instance of
            them, e.g. `get_super_block`, `block_device`

    @see init_bcache
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block device.
 */
static const BlockDevice *device; 

/**
    @brief global lock for block cache.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory block.

    We use a linked list to manage all allocated cached blocks.

    You can implement your own data structure if you like better performance.

    @see Block
 */
static ListNode head;

static LogHeader header; // in-memory copy of log header block.

/**
    @brief a struct to maintain other logging states.
    
    You may wonder where we store some states, e.g.
    
    * how many atomic operations are running?
    * are we checkpointing?
    * how to notify `end_op` that a checkpoint is done?

    Put them here!

    @see cache_begin_op, cache_end_op, cache_sync
 */

struct {
    /* your fields here */
    int commiting;
    usize used;
    usize num_running_ctx;
    SpinLock lock;
    Semaphore used_change;
    Semaphore checkpointed;
} log;

// read the content from disk.
static INLINE void device_read(Block *block) {
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block) {
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8 *)&header);
}

// initialize a block struct.
static void init_block(Block *block) {
    block->block_no = 0;
    block->ref = 0;
    init_list_node(&block->node);
    block->acquired = false;
    block->pinned = false;

    init_sleeplock(&block->lock);
    block->valid = false;
    memset(block->data, 0, sizeof(block->data));
}

// see `cache.h`.
static usize get_num_cached_blocks() {
    // TODO
    _acquire_spinlock(&lock);
    usize count = bcache.num_cached_blocks;
    _release_spinlock(&lock);
    return count;
}

// see `cache.h`.
static Block *cache_acquire(usize block_no) {
    // TODO
    Block* ret = NULL;
    _acquire_spinlock(&lock);
    _for_in_list(p, &head){
        if(p == &head)continue;
        Block* block = container_of(p, Block, node);
        if(block->block_no == block_no){
            block->ref++;
            ret = block;
            _detach_from_list(p);
            break;
        }
    }

    if(ret == NULL){
        ret = (Block*)kalloc(sizeof(Block));
        init_block(ret);
        ret->block_no = block_no;
        _release_spinlock(&lock);
        device_read(ret);
        _acquire_spinlock(&lock);
        ret->valid = true;
        bcache.num_cached_blocks++;
        ret->ref++;
    }
    _insert_into_list(&head, &ret->node);
    _release_spinlock(&lock);
    if(!wait_sem(&ret->lock))return NULL;   // return NULL indicates killed
    ret->acquired = true;
    return ret;
}

// see `cache.h`.
static void cache_release(Block *block) {
    // TODO
    _acquire_spinlock(&lock);
    block->ref--;
    block->acquired = false;
    if(bcache.num_cached_blocks > EVICTION_THRESHOLD){
        for(ListNode* p = head.prev; p != &head;){
            Block* block = container_of(p, Block, node);
            if(block->ref == 0 && !block->pinned){
                ListNode* prev = p->prev;
                _detach_from_list(p);
                kfree(block);
                bcache.num_cached_blocks--;
                p = prev;
            }
            else p = p->prev;
            if(bcache.num_cached_blocks < EVICTION_THRESHOLD)break;
        }
    }
    _release_spinlock(&lock);
    post_sem(&block->lock);
}


static void _write_log_area_back(){
    u8* buffer = (u8*)kalloc(BLOCK_SIZE);
    for(usize i = 0; i < header.num_blocks; i++){
        device->read(sblock->log_start + 1 + i, buffer);
        device->write(header.block_no[i], buffer);
    }
    header.num_blocks = 0;
    kfree(buffer);
}

// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device) {
    sblock = _sblock;
    device = _device;

    // TODO
    read_header();
    _write_log_area_back();
    write_header();

    init_spinlock(&lock);
    init_list_node(&head);
    bcache.num_cached_blocks = 0;

    init_sem(&log.used_change, 0);
    init_sem(&log.checkpointed, 0);
    init_spinlock(&log.lock);
    log.commiting = 0;
    log.used = 0;
    log.num_running_ctx = 0;
}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx) {
    // TODO
    if(!ctx)PANIC();
    while(1){
        _acquire_spinlock(&log.lock);
        usize used = log.used;
        int com = log.commiting;
        _release_spinlock(&log.lock);
        if(used + OP_MAX_NUM_BLOCKS > LOG_MAX_SIZE || com){
            if(!wait_sem(&log.used_change)){
                ctx->rm = -1;   //indicates having been killed
                return;
            }
        }
        else break;
    }
    _acquire_spinlock(&log.lock);
    log.used += OP_MAX_NUM_BLOCKS;
    log.num_running_ctx++;
    _release_spinlock(&log.lock);
    ctx->rm = OP_MAX_NUM_BLOCKS;
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block) {
    // TODO
    if(!ctx)device_write(block);
    else {
        usize i;
        _acquire_spinlock(&lock);
        for(i = 0; i < header.num_blocks; i++){
            if(header.block_no[i] == block->block_no)
                break;
        }
        if(i == header.num_blocks){
            if(ctx->rm == 0)PANIC();
            block->pinned = true;
            header.block_no[header.num_blocks++] = block->block_no;
            ctx->rm--;
        }
        _release_spinlock(&lock);
    }
}

static void _write_to_log_area(){
    for(usize i = 0; i < header.num_blocks; i++){
        auto from = bcache.acquire(header.block_no[i]);
        device->write(sblock->log_start + 1 + i, from->data);
        bcache.release(from);
    }
}

// see `cache.h`.
static void cache_end_op(OpContext *ctx) {
    // TODO
    bool commit_avail = false;
    _acquire_spinlock(&log.lock);
    log.num_running_ctx--;
    if(log.commiting)PANIC();
    if(!log.num_running_ctx){
        log.commiting = 1;
        commit_avail = true;
    }
    else{
        log.used -= ctx->rm;
        post_all_sem(&log.used_change);
    }
    if(!commit_avail){
        _lock_sem(&log.checkpointed);
        _release_spinlock(&log.lock);
        if(_wait_sem(&log.checkpointed, false))return;
    }
    else{
        _write_to_log_area();
        _release_spinlock(&log.lock);
        write_header();
        _acquire_spinlock(&log.lock);
        _write_log_area_back();        
        _release_spinlock(&log.lock);
        write_header();
        _acquire_spinlock(&log.lock);
        

        log.used = 0;
        log.commiting = 0;
        post_all_sem(&log.used_change);
        _release_spinlock(&log.lock);
        post_all_sem(&log.checkpointed);
    }
}

// see `cache.h`.
static usize cache_alloc(OpContext *ctx) {
    // TODO
    bool panic = false;
    for(usize bit_map_idx = 0; bit_map_idx < (BIT_PER_BLOCK - 1 + sblock->num_blocks) / BIT_PER_BLOCK; bit_map_idx++){
        auto bitmap_block = bcache.acquire(sblock->bitmap_start + bit_map_idx);
        for(usize i = 0; i < BLOCK_SIZE; i++){
            if(bitmap_block->data[i] != 0xff){
                usize bno = bit_map_idx * BIT_PER_BLOCK + 8 * i;
                u8 m = 0x1;
                while(m & bitmap_block->data[i]){
                    m <<= 1;
                    bno++;
                }
                if(bno == sblock->num_blocks){
                    panic = true;
                    break;
                }
                bitmap_block->data[i] |= m;
                bcache.sync(ctx, bitmap_block);
                bcache.release(bitmap_block);

                auto alloc = bcache.acquire(bno);
                memset(alloc->data, 0, BLOCK_SIZE);
                bcache.sync(ctx, alloc);
                bcache.release(alloc);
                return bno;
            }
        }
        bcache.release(bitmap_block);
        if(panic)PANIC();
    }
    PANIC();
}

// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no) {
    // TODO
    usize bitmap_idx = block_no / BIT_PER_BLOCK;
    usize location = block_no % BIT_PER_BLOCK;
    auto bitmap_block = bcache.acquire(sblock->bitmap_start + bitmap_idx);
    bitmap_block->data[location / 8] &= ~(0x1 << (location % 8));
    bcache.sync(ctx, bitmap_block);
    bcache.release(bitmap_block);
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};