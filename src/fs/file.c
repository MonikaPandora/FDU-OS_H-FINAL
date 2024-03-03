#include "file.h"
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/sem.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <common/list.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <common/bitmap.h>
#include <kernel/printk.h>
#include <kernel/sched.h>

// the global file table.
static struct ftable ftable;

void init_ftable() {
    // TODO: initialize your ftable.
    init_spinlock(&ftable.lock);
}

void init_oftable(struct oftable *oftable) {
    // TODO: initialize your oftable for a new process.
    memset(oftable, NULL, NOFILE);
}

/* Allocate a file structure. */
struct file* file_alloc() {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    for(int i = 0; i < NFILE; i++){
        if(ftable.files[i].ref == 0 && ftable.files[i].type == FD_NONE){
            ftable.files[i].ref = 1;
            _release_spinlock(&ftable.lock);
            return &ftable.files[i];
        }
    }
    _release_spinlock(&ftable.lock);
    return NULL;
}

/* Increment ref count for file f. */
struct file* file_dup(struct file* f) {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    f->ref++;
    _release_spinlock(&ftable.lock);
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file* f) {
    /* TODO: LabFinal */
    _acquire_spinlock(&ftable.lock);
    if(--f->ref <= 0){
        if(f->type == FD_PIPE){
            pipeClose(f->pipe, f->writable);
        }
        else if(f->type == FD_INODE){
            if(f->ip){
                _release_spinlock(&ftable.lock);
                OpContext ctx;
                bcache.begin_op(&ctx);
                inodes.put(&ctx, f->ip);
                bcache.end_op(&ctx);
                _acquire_spinlock(&ftable.lock);
            }
        }
        f->type = FD_NONE;
        f->ref = 0;
        f->readable = f->writable = 0;
    }
    _release_spinlock(&ftable.lock);
}

/* Get metadata about file f. */
int file_stat(struct file* f, struct stat* st) {
    /* TODO: LabFinal */
    if(f->type != FD_INODE)return -1;
    inodes.lock(f->ip);
    stati(f->ip, st);
    inodes.unlock(f->ip);
    return 0;
}

/* Read from file f. */
isize file_read(struct file* f, char* addr, isize n) {
    /* TODO: LabFinal */
    if(!f->readable || f->type == FD_NONE)return -1;
    isize ret = 0;
    if(f->type == FD_INODE){
        inodes.lock(f->ip);
        ret = (isize)inodes.read(f->ip, (u8*)addr, f->off, n);
        f->off += ret;
        inodes.unlock(f->ip);
    }
    else if(f->type == FD_PIPE){
        ret = pipeRead(f->pipe, (u64)addr, n);
    }
    return ret;
}


/* Write to file f. */
isize file_write(struct file* f, char* addr, isize n) {
    /* TODO: LabFinal */
    if(!f->writable || f->type == FD_NONE || n < 0)return -1;
    isize ret = 0;
    if(f->type == FD_INODE){
        ASSERT(f->ip->inode_no > 9);
        usize wsz = MIN(INODE_MAX_BYTES - f->off, (usize)n);
        usize n_w = 0;
        while(n_w != wsz){
            usize this = MIN(wsz - n_w, (usize)(OP_MAX_NUM_BLOCKS * BLOCK_SIZE / 2));
            OpContext ctx;
            bcache.begin_op(&ctx);
            inodes.lock(f->ip);
            if(inodes.write(&ctx, f->ip, (u8*)(addr + n_w), f->off, this) != this){
                inodes.unlock(f->ip);
                bcache.end_op(&ctx);
                return -1;
            };
            inodes.unlock(f->ip);
            bcache.end_op(&ctx);
            f->off += this;
            n_w += this;
            ret += this;
        }
    }
    else if(f->type == FD_PIPE){
        ret = pipeWrite(f->pipe, (u64)addr, n);
    }
    return ret;
}

usize get_file_ref(struct file* f){
    _acquire_spinlock(&ftable.lock);
    auto ret = f->ref;
    _release_spinlock(&ftable.lock);
    return ret;
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f) {
    /* TODO: Lab10 Shell */
    struct oftable* ft = &thisproc()->oftable;
    int i;
    for(i = 0; i < NOFILE; i++){
        if(ft->files[i] == NULL){
            ft->files[i] = f;
            break;
        }
    }
    return i == NOFILE ? -1 : i;
}

// get the file object by fd
// return null if the fd is invalid
struct file *fd2file(int fd) {
    // TODO
    if(fd < 0 || fd >= NOFILE)return NULL;
    else return thisproc()->oftable.files[fd];
}