#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <sys/stat.h>
#include <kernel/sched.h>
#include <kernel/console.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock* sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache* cache;

/**
    @brief global lock for inode layer.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, ref counts, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory inodes.

    We use a linked list to manage all allocated inodes.

    You can implement your own data structure if you want better performance.

    @see Inode
 */
static ListNode head;

// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry* get_entry(Block* block, usize inode_no) {
    return ((InodeEntry*)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32* get_addrs(Block* block) {
    return ((IndirectBlock*)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock* _sblock, const BlockCache* _cache) {
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes){
        inodes.root = inodes.get(ROOT_INODE_NO);
    }
    else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode* inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext* ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);
    // TODO
    usize root_block_idx = to_block_no(ROOT_INODE_NO) - sblock->inode_start;
    usize inode_no = ROOT_INODE_NO;
    bool panic_flag = true;
    for(usize i = root_block_idx; i < sblock->num_inodes * sizeof(InodeEntry) / BLOCK_SIZE && panic_flag; i++){
        auto block = cache->acquire(sblock->inode_start + i);
        for(usize j = (i != root_block_idx ? 0 : ROOT_INODE_NO + 1); j < BLOCK_SIZE / sizeof(InodeEntry); j++){
            auto entry = get_entry(block, j);
            inode_no++;
            if(entry->type == INODE_INVALID){
                memset(entry, 0, sizeof(InodeEntry));
                entry->type = type;
                cache->sync(ctx, block);
                panic_flag = false;
                break;
            }
        }
        cache->release(block);
    }
    if(panic_flag)PANIC();
    return inode_no;
}

static void inode_sync(OpContext* ctx, Inode* inode, bool do_write);

// see `inode.h`.
static void inode_lock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    if(!wait_sem(&inode->lock)){
        inode->valid = false; // if inode->valid is false after calling this function, it means been killed
        return;
    }
    else if(inode->valid == false){
        inode_sync(NULL, inode, false);
    }
}

// see `inode.h`.
static void inode_unlock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    post_sem(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext* ctx, Inode* inode, bool do_write) {
    // TODO
    if(do_write){
        if(inode->valid == false)PANIC();
        else{
            usize block_no = to_block_no(inode->inode_no);
            auto block = cache->acquire(block_no);
            memcpy(get_entry(block, inode->inode_no), &inode->entry, sizeof(InodeEntry));
            cache->sync(ctx, block);
            cache->release(block);
        }
    }
    else{
        if(inode->valid == false){
            usize block_no = to_block_no(inode->inode_no);
            auto block = cache->acquire(block_no);
            memcpy(&inode->entry, get_entry(block, inode->inode_no), sizeof(InodeEntry));
            cache->release(block);
            inode->valid = true;
        }
    }
}

// see `inode.h`.
static Inode* inode_get(usize inode_no) {
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    // TODO
    Inode* ret = NULL;
    _acquire_spinlock(&lock);
    _for_in_list(p, &head){
        if(p == &head)continue;
        Inode* inode = container_of(p, Inode, node);
        if(inode->inode_no == inode_no){
            _increment_rc(&inode->rc);
            ret = inode;
            _detach_from_list(p);
            break;
        }
    }

    if(ret == NULL){
        ret = (Inode*)kalloc(sizeof(Inode));
        init_inode(ret);
        ret->inode_no = inode_no;
        _increment_rc(&ret->rc);
    }
    _insert_into_list(&head, &ret->node);
    _release_spinlock(&lock);
    return ret;
}

void check_op_ctx(OpContext* ctx){
    if(ctx->rm == 0){
        cache->end_op(ctx);
        cache->begin_op(ctx);
    }
}

// see `inode.h`.
static void inode_clear(OpContext* ctx, Inode* inode) {
    // TODO
    usize num_blocks = (inode->entry.num_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for(usize i = 0; i < INODE_NUM_DIRECT && num_blocks; i++, num_blocks--){
        // check_op_ctx(ctx);
        cache->free(ctx, inode->entry.addrs[i]);
        inode->entry.addrs[i] = 0;
    }
    if(inode->entry.indirect){
        auto indirect_block = cache->acquire(inode->entry.indirect);
        u32* addrs = get_addrs(indirect_block);
        for(usize i = 0; i < INODE_NUM_INDIRECT && num_blocks; i++, num_blocks--){
            // check_op_ctx(ctx);
            cache->free(ctx, addrs[i]);
        }
        // check_op_ctx(ctx);
        cache->release(indirect_block);
        cache->free(ctx, inode->entry.indirect); //free indirect block 
    }
    // reset some metadata
    inode->entry.num_bytes = 0;
    inode->entry.indirect = 0;
    inode_sync(ctx, inode, true);
}

// see `inode.h`.
static Inode* inode_share(Inode* inode) {
    // TODO
    _increment_rc(&inode->rc);
    return inode;
}

// see `inode.h`.
static void inode_put(OpContext* ctx, Inode* inode) {
    // TODO
    inode_lock(inode);
    if(inode->rc.count == 1 && inode->entry.num_links == 0){
        if(inode->entry.num_bytes)inode_clear(ctx, inode);
        inode->entry.type = INODE_INVALID;
        inode_sync(ctx, inode, true);
        _acquire_spinlock(&lock);
        _detach_from_list(&inode->node);
        _release_spinlock(&lock);
        inode_unlock(inode);
        kfree(inode);
        return;
    }
    inode_unlock(inode);
    _decrement_rc(&inode->rc);
}

/**
    @brief get which block is the offset of the inode in.

    e.g. `inode_map(ctx, my_inode, 1234, &modified)` will return the block_no
    of the block that contains the 1234th byte of the file
    represented by `my_inode`.

    If a block has not been allocated for that byte, `inode_map` will
    allocate a new block and update `my_inode`, at which time, `modified`
    will be set to true.

    HOWEVER, if `ctx == NULL`, `inode_map` will NOT try to allocate any new block,
    and when it finds that the block has not been allocated, it will return 0.
    
    @param[out] modified true if some new block is allocated and `inode`
    has been changed.

    @return usize the block number of that block, or 0 if `ctx == NULL` and
    the required block has not been allocated.

    @note the caller must hold the lock of `inode`.
 */
static usize inode_map(OpContext* ctx,
                       Inode* inode,
                       usize offset,
                       bool* modified) {
    // TODO
    usize b_idx = offset / BLOCK_SIZE;
    ASSERT(b_idx < INODE_MAX_BLOCKS);
    usize ret = 0;
    *modified = false;
    if(b_idx < INODE_NUM_DIRECT){
        if(inode->entry.addrs[b_idx] == 0){
            if(ctx){
                *modified = true;
                inode->entry.addrs[b_idx] = cache->alloc(ctx);
            }
        }
        ret = inode->entry.addrs[b_idx];
    }
    else {
        b_idx = b_idx - INODE_NUM_DIRECT;
        if(inode->entry.indirect == 0){
            if(ctx){
                *modified = true;
                inode->entry.indirect = cache->alloc(ctx);
            }
            else return 0;
        }
        auto ib = cache->acquire(inode->entry.indirect);
        auto addrs = get_addrs(ib);
        if(addrs[b_idx] == 0){
            if(ctx){
                *modified = true;
                addrs[b_idx] = cache->alloc(ctx);
                cache->sync(ctx, ib);
            }
        }
        ret = addrs[b_idx];
        cache->release(ib);
    }
    return ret;
}

static usize _inode_rw (OpContext* ctx, 
                        Inode* inode, 
                        u8* buffer, 
                        usize offset, 
                        usize count, 
                        bool do_write){
    if(do_write && ctx == NULL)PANIC();
    usize end = offset + count;
    usize has_done = 0;
    bool modified;
    while(offset != end){
        usize cur_bno = inode_map(ctx, inode, offset, &modified);
        if(cur_bno == 0)PANIC();
        if(do_write == false)ASSERT(modified == false);

        auto block = cache->acquire(cur_bno);
        usize size = MIN((BLOCK_SIZE - offset % BLOCK_SIZE), end - offset);
        if(do_write){
            memcpy(block->data + offset % BLOCK_SIZE, buffer + has_done, size);
            // check_op_ctx(ctx);
            cache->sync(ctx, block);
        }
        else memcpy(buffer + has_done, block->data + offset % BLOCK_SIZE, size);
        cache->release(block);

        offset += size;
        has_done += size;
    }
    ASSERT(has_done == count);
    if(do_write && inode->entry.num_bytes < end){
        inode->entry.num_bytes = end;
    }
    return count;
}

// see `inode.h`.
static usize inode_read(Inode* inode, u8* dest, usize offset, usize count) {
    if(inode->entry.type == INODE_DEVICE){
        return console_read(inode, (char*)dest, count);
    }
    
    InodeEntry* entry = &inode->entry;
    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);

    if(offset == entry->num_bytes)ASSERT(count == 0);
    // TODO
    return _inode_rw(NULL, inode, dest, offset, count, false);
}

// see `inode.h`.
static usize inode_write(OpContext* ctx,
                         Inode* inode,
                         u8* src,
                         usize offset,
                         usize count) {
    if(inode->entry.type == INODE_DEVICE){
        return console_write(inode, (char*)src, count);
    }

    InodeEntry* entry = &inode->entry;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    // TODO
    ASSERT(offset < INODE_MAX_BYTES);

    auto ret = _inode_rw(ctx, inode, src, offset, count, true);
    inode_sync(ctx, inode, true);
    return ret;
}

// see `inode.h`.
static usize inode_lookup(Inode* inode, const char* name, usize* index) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    if(entry->num_bytes){
        DirEntry* des = (DirEntry*)kalloc(entry->num_bytes);
        inode_read(inode, (u8*)des, 0, entry->num_bytes);
        for(usize i = 0; i < entry->num_bytes / sizeof(DirEntry); i++){
            if(strlen(name) == strlen((const char*)&des[i].name) && 
                strncmp(name, (const char*)&des[i].name, strlen(name) + 1) == 0){
                auto ret = des[i].inode_no;
                kfree(des);
                if(index)*index = i;
                return ret;
            }
        }
        kfree(des);
    }
    return 0;
}

// see `inode.h`.
static usize inode_insert(OpContext* ctx,
                          Inode* inode,
                          const char* name,
                          usize inode_no) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);
    // TODO
    usize index;
    if(inode_lookup(inode, name, &index))return -1;
    DirEntry de;
    de.inode_no = inode_no;
    strncpy(de.name, name, MIN((strlen(name) + 1), (usize)FILE_NAME_MAX_LENGTH));
    auto resp = inode_write(ctx, inode, (u8*)&de, entry->num_bytes, sizeof(DirEntry));
    if(resp != sizeof(DirEntry))return -1;
    return index;
}

// see `inode.h`.
static void inode_remove(OpContext* ctx, Inode* inode, usize index) {
    // TODO
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);
    DirEntry de;
    if(index < entry->num_bytes / sizeof(DirEntry)){
        if(index < entry->num_bytes / sizeof(DirEntry) - 1){
            inode_read(inode, (u8*)&de, entry->num_bytes - sizeof(DirEntry), sizeof(DirEntry));
            inode_write(ctx, inode, (u8*)&de, index * sizeof(DirEntry), sizeof(DirEntry));
        }
        entry->num_bytes -= sizeof(DirEntry);
        inode_sync(ctx, inode, true);
    }
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};



/* LabFinal */

/**
    @brief read the next path element from `path` into `name`.
    
    @param[out] name next path element.

    @return const char* a pointer offseted in `path`, without leading `/`. If no
    name to remove, return NULL.

    @example 
    skipelem("a/bb/c", name) = "bb/c", setting name = "a",
    skipelem("///a//bb", name) = "bb", setting name = "a",
    skipelem("a", name) = "", setting name = "a",
    skipelem("", name) = skipelem("////", name) = NULL, not setting name.
 */
static const char* skipelem(const char* path, char* name) {
    const char* s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
    @brief look up and return the inode for `path`.

    If `nameiparent`, return the inode for the parent and copy the final
    path element into `name`.
    
    @param path a relative or absolute path. If `path` is relative, it is
    relative to the current working directory of the process.

    @param[out] name the final path element if `nameiparent` is true.

    @return Inode* the inode for `path` (or its parent if `nameiparent` is true), 
    or NULL if such inode does not exist.

    @example
    namex("/a/b", false, name) = inode of b,
    namex("/a/b", true, name) = inode of a, setting name = "b",
    namex("/", true, name) = NULL (because "/" has no parent!)
 */
static Inode* namex(const char* path,
                    bool nameiparent,
                    char* name,
                    OpContext* ctx) {
    /* TODO: LabFinal */
    if(strncmp(path, "/", 2) == 0){
        return inodes.get(inodes.root->inode_no);
    }
    Inode* ret;
    if(path[0] == '.' || path[0] != '/')ret = inodes.get(thisproc()->cwd->inode_no);
    else ret = inodes.get(inodes.root->inode_no);

    usize index;
    name[0] = 0;
    path = skipelem(path, name);
    if(path == NULL){
        inodes.put(ctx, ret);
        return NULL;
    }
    while(path[0] != '\0'){
        inodes.lock(ret);
        usize ino = inodes.lookup(ret, name, &index);
        inodes.unlock(ret);
        inodes.put(ctx, ret);
        if(ino == 0)return NULL;
        ret = inodes.get(ino);
        path = skipelem(path, name);
    }
    if(!nameiparent){
        inodes.lock(ret);
        usize ino = inodes.lookup(ret, name, &index);
        inodes.unlock(ret);
        inodes.put(ctx, ret);
        if(ino == 0)return NULL;
        ret = inodes.get(ino);
        name = NULL;
    }
    return ret;
}

Inode* namei(const char* path, OpContext* ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode* nameiparent(const char* path, char* name, OpContext* ctx) {
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.
    
    @note the caller must hold the lock of `ip`.
 */
void stati(Inode* ip, struct stat* st) {
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
        case INODE_REGULAR:
            st->st_mode = S_IFREG;
            break;
        case INODE_DIRECTORY:
            st->st_mode = S_IFDIR;
            break;
        case INODE_DEVICE:
            st->st_mode = 0;
            break;
        default:
            PANIC();
    }
}