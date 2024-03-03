//
// File-system system calls implementation.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include "syscall.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

struct iovec {
    void *iov_base; /* Starting address. */
    usize iov_len;  /* Number of bytes to transfer. */
};

// ioctl - control device
define_syscall(ioctl, int fd, u64 request) {
    // 0x5413 is TIOCGWINSZ (I/O Control to Get the WINdow SIZe, a magic request
    // to get the stdin terminal size) in our implementation. Just ignore it.
    ASSERT(request == 0x5413);
    (void)fd;
    return 0;
}

void get_free_vm(struct pgdir* pd, u64 length, u64* begin, u64* end){
    // get vm area between heap and userstack section
    // a file must auto-mmapped at the top of the area
    *end = (u64)-1;
    _for_in_list(p, &pd->section_head){
        if(p != &pd->section_head){
            auto st = container_of(p, struct section, stnode);
            if(st->flags == ST_HEAP)*begin = st->end;
            if(st->flags == ST_MMAP_PRIVATE 
                || st->flags == ST_MMAP_SHARED
                || st->flags == ST_USER_STACK)*end = MIN(*end, st->begin);
        }
    }
    if(*end - *begin < length){
        // to fix
        *begin = *end = 0;
    }
    else *begin = *end - length;
}

// mmap - map files or devices into memory
define_syscall(mmap, void *addr, int length, int prot, int flags, int fd,
               int offset) {
    // TODO
    if(prot == PROT_NONE || prot&PROT_EXEC || fd < 0 || fd >= NOFILE || length <= 0)return -1;
    auto st = (struct section*)kalloc(sizeof(struct section));
    memset(st, 0, sizeof(struct section));
    st->flags = flags == MAP_SHARED ? ST_MMAP_SHARED : ST_MMAP_PRIVATE;

    auto this = thisproc();
    auto f = fd2file(fd);
    if(!f){
        kfree(st);
        return -1;
    }

    if((prot & PROT_WRITE) && !f->writable && flags != MAP_PRIVATE){
        kfree(st);
        return -1;
    }

    st->fp = file_dup(f);

    _acquire_spinlock(&this->pgdir.lock);
    if(addr == 0){
        u64 free_begin, free_end;
        get_free_vm(&this->pgdir, length, &free_begin, &free_end);
        if(free_end == free_begin){
            // can not find an area
            kfree(st);
            _release_spinlock(&this->pgdir.lock);
            return -1;
        }
        st->end = free_end;
        st->begin = st->end - (u64)length;
    }
    else{
        _for_in_list(p, &this->pgdir.section_head){
            if(p != &this->pgdir.section_head){
                auto sec = container_of(p, struct section, stnode);
                if(sec->begin < (u64)addr + (u64)length && (u64)addr < sec->end){
                    kfree(st);
                    _release_spinlock(&this->pgdir.lock);
                    return -1;
                }
            }
        }
        st->begin = (u64)addr;
        st->end = st->begin + (u64)length;
    }
    st->length = (u64)length;
    st->offset = (int)offset;
    _insert_into_list(&this->pgdir.section_head, &st->stnode);
    st->prot = prot;
    _release_spinlock(&this->pgdir.lock);
    return st->begin;
}

// munmap - unmap files or devices into memory
define_syscall(munmap, void *addr, u64 length) {
    // TODO
    auto this = thisproc();
    _acquire_spinlock(&this->pgdir.lock);
    _for_in_list(p, &this->pgdir.section_head){
        if(p != &this->pgdir.section_head){
            auto st = container_of(p, struct section, stnode);
            if((u64)addr == st->begin){
                ASSERT(st->flags == ST_MMAP_PRIVATE || st->flags == ST_MMAP_SHARED);
                ASSERT(st->fp);
                if(length >= st->end - st->begin){
                    free_section_pages(&this->pgdir, st);
                    _detach_from_list(p);
                    file_close(st->fp);
                    kfree(st);
                }
                else {
                    auto end = st->begin + length;
                    for(auto i = PAGE_BASE(st->begin); i < end; i += PAGE_SIZE){
                        auto pte = get_pte(&this->pgdir, i, false);
                        if(st->fp->type == FD_INODE && get_page_ref(P2K(PTE_ADDRESS(*pte))) == 1){
                            u64 this_begin = MAX(i, st->begin);
                            u64 this_end = MIN(i + PAGE_SIZE, end);
                            st->fp->off = st->offset + this_begin - st->begin;
                            file_write(st->fp, (char*)P2K(PTE_ADDRESS(*pte)), MIN((u64)PAGE_SIZE, this_end - this_begin));
                        }
                        else if(st->fp->type == FD_PIPE){
                            // TODO
                            PANIC();
                        }
                        else PANIC();
                        kfree_page((void*)P2K(PTE_ADDRESS(*pte)));
                        *pte = NULL;
                    }
                    st->begin = end;
                }
                break;
            }
        }
    }
    _release_spinlock(&this->pgdir.lock);
    return 0;
}

// dup - duplicate a file descriptor
define_syscall(dup, int fd) {
    struct file *f = fd2file(fd);
    if (!f)
        return -1;
    fd = fdalloc(f);
    if (fd < 0)
        return -1;
    file_dup(f);
    return fd;
}

// read - read from a file descriptor
define_syscall(read, int fd, char *buffer, int size) {
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_writeable(buffer, size))
        return -1;
    return file_read(f, buffer, size);
}

// write - write to a file descriptor
define_syscall(write, int fd, char *buffer, int size) {
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size))
        return -1;
    return file_write(f, buffer, size);
}

// writev - write data into multiple buffers
define_syscall(writev, int fd, struct iovec *iov, int iovcnt) {
    struct file *f = fd2file(fd);
    struct iovec *p;
    if (!f || iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt))
        return -1;
    usize tot = 0;
    for (p = iov; p < iov + iovcnt; p++) {
        if (!user_readable(p->iov_base, p->iov_len))
            return -1;
        tot += file_write(f, p->iov_base, p->iov_len);
    }
    return tot;
}

// close - close a file descriptor
define_syscall(close, int fd) {
    /* TODO: LabFinal */
    if(fd < 0 || fd >= NOFILE)return -1;
    auto ft = &thisproc()->oftable;
    if(ft->files[fd]){
        file_close(ft->files[fd]);
        ft->files[fd] = NULL;
    }
    return 0;
}

// fstat - get file status
define_syscall(fstat, int fd, struct stat *st) {
    struct file *f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st)))
        return -1;
    return file_stat(f, st);
}

// newfstatat - get file status (on some platform also called fstatat64, i.e. a
// 64-bit version of fstatat)
define_syscall(newfstatat, int dirfd, const char *path, struct stat *st,
               int flags) {
    if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st)))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_fstatat: dirfd unimplemented\n");
        return -1;
    }
    if (flags != 0) {
        printk("sys_fstatat: flags unimplemented\n");
        return -1;
    }

    Inode *ip;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = namei(path, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.lock(ip);
    stati(ip, st);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);

    return 0;
}

// is the directory `dp` empty except for "." and ".." ?
static int isdirempty(Inode *dp) {
    usize off;
    DirEntry de;

    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
            PANIC();
        if (de.inode_no != 0)
            return 0;
    }
    return 1;
}

// unlinkat - delete a name and possibly the file it refers to
define_syscall(unlinkat, int fd, const char *path, int flag) {
    ASSERT(fd == AT_FDCWD && flag == 0);
    Inode *ip, *dp;
    // DirEntry de;
    char name[FILE_NAME_MAX_LENGTH];
    usize index;
    if (!user_strlen(path, 256))
        return -1;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((dp = nameiparent(path, name, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(dp);

    // Cannot unlink "." or "..".
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
        strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
        goto bad;

    usize inumber = inodes.lookup(dp, name, &index);
    if (inumber == 0)
        goto bad;
    ip = inodes.get(inumber);
    inodes.lock(ip);

    if (ip->entry.num_links < 1)
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        goto bad;
    }

    // memset(&de, 0, sizeof(de));
    // if (inodes.write(&ctx, dp, (u8 *)&de, off * sizeof(DirEntry), sizeof(de)) != sizeof(de))
    //     PANIC();
    inodes.remove(&ctx, dp, index);
    if (ip->entry.type == INODE_DIRECTORY) {
        dp->entry.num_links--;
        inodes.sync(&ctx, dp, true);
    }
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    ip->entry.num_links--;
    inodes.sync(&ctx, ip, true);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;

bad:
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    bcache.end_op(&ctx);
    return -1;
}

/**
    @brief create an inode at `path` with `type`.

    If the inode exists, just return it.

    If `type` is directory, you should also create "." and ".." entries and link
   them with the new inode.

    @note BE careful of handling error! You should clean up ALL the resources
   you allocated and free ALL acquired locks when error occurs. e.g. if you
   allocate a new inode "/my/dir", but failed to create ".", you should free the
   inode "/my/dir" before return.

    @see `nameiparent` will find the parent directory of `path`.

    @return Inode* the created inode, or NULL if failed.
 */
Inode *create(const char *path, InodeType type, short major, short minor,
              OpContext *ctx) {
    /* TODO: LabFinal */
    char name[FILE_NAME_MAX_LENGTH] = {0};
    usize index;
    Inode* parent = nameiparent(path, name, ctx);
    if(!parent)return NULL;
    inodes.lock(parent);
    usize ino = inodes.lookup(parent, name, &index);
    if(ino){
        inodes.unlock(parent);
        inodes.put(ctx, parent);
        return inodes.get(ino);
    }
    else{
        ino = inodes.alloc(ctx, type);
        if(ino == 0){
            inodes.unlock(parent);
            inodes.put(ctx, parent);
            return NULL;
        }
        Inode* node = inodes.get(ino);
        inodes.lock(node);
        node->entry.type = type;
        node->entry.major = major;
        node->entry.minor = minor;
        node->entry.num_links = 1;
        if(type == INODE_DIRECTORY){
            node->entry.num_links++;
            ASSERT(inodes.insert(ctx, node, ".", ino) != (usize)-1);
            ASSERT(inodes.insert(ctx, node, "..", parent->inode_no) != (usize)-1);
        }
        inodes.sync(ctx, node, true);

        ASSERT(inodes.insert(ctx, parent, name, ino) != (usize)-1);
        inodes.unlock(parent);
        inodes.put(ctx, parent);
        return node;
    }
}

// openat - open a file
define_syscall(openat, int dirfd, const char *path, int omode) {
    int fd;
    struct file *f;
    Inode *ip;

    if (!user_strlen(path, 256))
        return -1;

    if (dirfd != AT_FDCWD) {
        printk("sys_openat: dirfd unimplemented\n");
        return -1;
    }

    OpContext ctx;
    bcache.begin_op(&ctx);
    if (omode & O_CREAT) {
        // FIXME: Support acl mode.
        ip = create(path, INODE_REGULAR, 0, 0, &ctx);
        if (ip == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
    } else {
        if ((ip = namei(path, &ctx)) == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
        inodes.lock(ip);
    }

    if ((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            file_close(f);
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    bcache.end_op(&ctx);

    f->type = FD_INODE;
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

// mkdirat - create a directory
define_syscall(mkdirat, int dirfd, const char *path, int mode) {
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mkdirat: dirfd unimplemented\n");
        return -1;
    }
    if (mode != 0) {
        printk("sys_mkdirat: mode unimplemented\n");
        return -1;
    }
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

// mknodat - create a special or ordinary file
define_syscall(mknodat, int dirfd, const char *path, mode_t mode, dev_t dev) {
    if(mode){}
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mknodat: dirfd unimplemented\n");
        return -1;
    }

    unsigned int ma = major(dev);
    unsigned int mi = minor(dev);
    printk("mknodat: path '%s', major:minor %u:%u\n", path, ma, mi);
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DEVICE, (short)ma, (short)mi, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

// chdir - change current working directory
define_syscall(chdir, const char *path) {
    // TODO
    // change the cwd (current working dictionary) of current process to 'path'
    // you may need to do some validations
    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode* node = namei(path, &ctx);
    if(node){
        inodes.put(&ctx, thisproc()->cwd);
        bcache.end_op(&ctx);
        thisproc()->cwd = node;
        return 0;
    }
    else {
        bcache.end_op(&ctx);
        return -1;
    }
}

// pipe2 - create a pipe
define_syscall(pipe2, int pipefd[2], int flags) {
    // TODO
    // you can ignore the flags here,
    // or if you like, do some assertions to filter out unimplemented flags
    (void)flags;
    File *f0, *f1;
    if(pipeAlloc(&f0, &f1) < 0)return -1;
    if((pipefd[0] = fdalloc(f0)) < 0){
        pipeClose(f0->pipe, 0);
        pipeClose(f0->pipe, 1);
        file_close(f0);
        file_close(f1);
        return -1;
    }
    if((pipefd[1] = fdalloc(f1)) < 0){
        pipeClose(f0->pipe, 0);
        pipeClose(f0->pipe, 1);
        sys_close(pipefd[0]);
        file_close(f1);
        return -1;
    }
    return 0;
}