#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

#define INODE_MAX_BLOCKS 140
#define INODE_MAX_BYTES (INODE_MAX_BLOCKS * 512)

/* from mmaptest */

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02

#define PGSIZE 4096
#define BSIZE 512
#define O_CREATE O_CREAT

char mmap_buf[BSIZE];

#define MAP_FAILED ((char*)-1)

char* testname = "???";

void err(char* why) {
    printf("mmaptest: %s failed: %s, pid=%d\n", testname, why, getpid());
    exit(1);
}

//
// check the content of the two mapped pages.
//
void _v1(char* p) {
    int i;
    for (i = 0; i < PGSIZE * 2; i++) {
        if (i < PGSIZE + (PGSIZE / 2)) {
            if (p[i] != 'A') {
                printf("mismatch at %d, wanted 'A', got 0x%x\n", i, p[i]);
                err("v1 mismatch (1)");
            }
        } else {
            if (p[i] != 0) {
                printf("mismatch at %d, wanted zero, got 0x%x\n", i, p[i]);
                err("v1 mismatch (2)");
            }
        }
    }
}

//
// create a file to be mapped, containing
// 1.5 pages of 'A' and half a page of zeros.
//
void makefile(const char* f) {
    int i;
    int n = PGSIZE / BSIZE;

    unlink(f);
    int fd = open(f, O_WRONLY | O_CREATE);
    if (fd == -1)
        err("open");
    memset(mmap_buf, 'A', BSIZE);
    // write 1.5 page
    for (i = 0; i < n + n / 2; i++) {
        if (write(fd, mmap_buf, BSIZE) != BSIZE)
            err("write 0 makefile");
    }
    if (close(fd) == -1)
        err("close");
}

void mmap_test(void) {
    int fd;
    int i;
    const char* const f = "mmap.dur";
    printf("mmap_test starting\n");
    testname = "mmap_test";

    //
    // create a file with known content, map it into memory, check that
    // the mapped memory has the same bytes as originally written to the
    // file.
    //
    makefile(f);
    if ((fd = open(f, O_RDONLY)) == -1)
        err("open");

    printf("test mmap f\n");
    //
    // this call to mmap() asks the kernel to map the content
    // of open file fd into the address space. the first
    // 0 argument indicates that the kernel should choose the
    // virtual address. the second argument indicates how many
    // bytes to map. the third argument indicates that the
    // mapped memory should be read-only. the fourth argument
    // indicates that, if the process modifies the mapped memory,
    // that the modifications should not be written back to
    // the file nor shared with other processes mapping the
    // same file (of course in this case updates are prohibited
    // due to PROT_READ). the fifth argument is the file descriptor
    // of the file to be mapped. the last argument is the starting
    // offset in the file.
    //
    char* p = mmap(0, PGSIZE * 2, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED)
        err("mmap (1)");
    _v1(p);
    if (munmap(p, PGSIZE * 2) == -1)
        err("munmap (1)");

    printf("test mmap f: OK\n");

    printf("test mmap private\n");
    // should be able to map file opened read-only with private writable
    // mapping
    p = mmap(0, PGSIZE * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED)
        err("mmap (2)");
    if (close(fd) == -1)
        err("close");
    _v1(p);
    for (i = 0; i < PGSIZE * 2; i++)
        p[i] = 'Z';
    if (munmap(p, PGSIZE * 2) == -1)
        err("munmap (2)");

    printf("test mmap private: OK\n");

    printf("test mmap read-only\n");

    // check that mmap doesn't allow read/write mapping of a
    // file opened read-only.
    if ((fd = open(f, O_RDONLY)) == -1)
        err("open");
    p = mmap(0, PGSIZE * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED)
        err("mmap call should have failed");
    if (close(fd) == -1)
        err("close");

    printf("test mmap read-only: OK\n");

    printf("test mmap read/write\n");

    // check that mmap does allow read/write mapping of a
    // file opened read/write.
    if ((fd = open(f, O_RDWR)) == -1)
        err("open");
    p = mmap(0, PGSIZE * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        err("mmap (3)");
    if (close(fd) == -1)
        err("close");

    // check that the mapping still works after close(fd).
    _v1(p);

    // write the mapped memory.
    for (i = 0; i < PGSIZE * 2; i++)
        p[i] = 'Z';

    // unmap just the first two of three pages of mapped memory.
    if (munmap(p, PGSIZE * 2) == -1)
        err("munmap (3)");

    printf("test mmap read/write: OK\n");

    printf("test mmap dirty\n");

    // check that the writes to the mapped memory were
    // written to the file.
    if ((fd = open(f, O_RDWR)) == -1)
        err("open");
    for (i = 0; i < PGSIZE + (PGSIZE / 2); i++) {
        char b;
        if (read(fd, &b, 1) != 1)
            err("read (1)");
        if (b != 'Z')
            err("file does not contain modifications");
    }
    if (close(fd) == -1)
        err("close");

    printf("test mmap dirty: OK\n");

    printf("test not-mapped unmap\n");

    // unmap the rest of the mapped memory.
    if (munmap(p + PGSIZE * 2, PGSIZE) == -1)
        err("munmap (4)");

    printf("test not-mapped unmap: OK\n");

    printf("test mmap two files\n");

    //
    // mmap two files at the same time.
    //
    int fd1;
    if ((fd1 = open("mmap1", O_RDWR | O_CREATE)) < 0)
        err("open mmap1");
    if (write(fd1, "12345", 5) != 5)
        err("write mmap1");
    char* p1 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd1, 0);
    if (p1 == MAP_FAILED)
        err("mmap mmap1");
    close(fd1);
    unlink("mmap1");

    int fd2;
    if ((fd2 = open("mmap2", O_RDWR | O_CREATE)) < 0)
        err("open mmap2");
    if (write(fd2, "67890", 5) != 5)
        err("write mmap2");
    char* p2 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd2, 0);
    if (p2 == MAP_FAILED)
        err("mmap mmap2");
    close(fd2);
    unlink("mmap2");

    if (memcmp(p1, "12345", 5) != 0)
        err("mmap1 mismatch");
    if (memcmp(p2, "67890", 5) != 0)
        err("mmap2 mismatch");

    munmap(p1, PGSIZE);
    if (memcmp(p2, "67890", 5) != 0)
        err("mmap2 mismatch (2)");
    munmap(p2, PGSIZE);

    printf("test mmap two files: OK\n");

    printf("mmap_test: ALL OK\n");
}

//
// mmap a file, then fork.
// check that the child sees the mapped file.
//
void fork_test(void) {
    int fd;
    int pid;
    const char* const f = "mmap.dur";

    printf("fork_test starting\n");
    testname = "fork_test";

    // mmap the file twice.
    makefile(f);
    if ((fd = open(f, O_RDONLY)) == -1)
        err("open");
    unlink(f);
    char* p1 = mmap(0, PGSIZE * 2, PROT_READ, MAP_SHARED, fd, 0);
    if (p1 == MAP_FAILED)
        err("mmap (4)");
    char* p2 = mmap(0, PGSIZE * 2, PROT_READ, MAP_SHARED, fd, 0);
    if (p2 == MAP_FAILED)
        err("mmap (5)");

    // read just 2nd page.
    if (*(p1 + PGSIZE) != 'A')
        err("fork mismatch (1)");

    if ((pid = fork()) < 0)
        err("fork");
    if (pid == 0) {
        _v1(p1);
        munmap(p1, PGSIZE);  // just the first page
        printf("fork_test child OK\n");
        exit(0);
    }

    // int status = -1;  // This status code doesn't work due to our implementation
    // wait(&status);
    wait(NULL);

    // check that the parent's mappings are still there.
    _v1(p1);
    _v1(p2);

    printf("fork_test parent OK\n");
}

/* end from mmaptest */

char buf[8192];
char name[3];

void opentest(void) {
    int fd;

    printf("open test\n");
    fd = open("echo", 0);
    if (fd < 0) {
        printf("open echo failed!\n");
        exit(1);
    }
    close(fd);
    fd = open("doesnotexist", 0);
    if (fd >= 0) {
        printf("open doesnotexist succeeded!\n");
        exit(0);
    }
    printf("open test ok\n");
}

void writetest(void) {
    int fd;
    int i;

    printf("small file test\n");
    fd = open("small", O_CREAT | O_RDWR);
    if (fd >= 0) {
        printf("creat small succeeded; ok\n");
    } else {
        printf("error: creat small failed!\n");
        exit(1);
    }
    for (i = 0; i < 100; i++) {
        if (write(fd, "aaaaaaaaaa", 10) != 10) {
            printf("error: write aa %d new file failed\n", i);
            exit(1);
        }
        if (write(fd, "bbbbbbbbbb", 10) != 10) {
            printf("error: write bb %d new file failed\n", i);
            exit(1);
        }
    }
    printf("writes ok\n");
    close(fd);
    fd = open("small", O_RDONLY);
    if (fd >= 0) {
        printf("open small succeeded ok\n");
    } else {
        printf("error: open small failed!\n");
        exit(1);
    }
    i = read(fd, buf, 2000);
    if (i == 2000) {
        printf("read succeeded ok\n");
    } else {
        printf("read failed\n");
        exit(1);
    }
    close(fd);

    if (unlink("small") < 0) {
        printf("unlink small failed\n");
        exit(1);
    }
    printf("small file test ok\n");
}

void writetestbig(void) {
    int i, fd, n;

    printf("big files test\n");

    fd = open("big", O_CREAT | O_RDWR);
    if (fd < 0) {
        printf("error: creat big failed!\n");
        exit(1);
    }

    for (i = 0; i < INODE_MAX_BLOCKS; i++) {
        ((int*)buf)[0] = i;
        if (write(fd, buf, 512) != 512) {
            printf("error: write big file failed\n");
            exit(1);
        }
    }

    close(fd);

    fd = open("big", O_RDONLY);
    if (fd < 0) {
        printf("error: open big failed!\n");
        exit(1);
    }

    n = 0;
    for (;;) {
        i = read(fd, buf, 512);
        if (i == 0) {
            if (n == INODE_MAX_BLOCKS - 1) {
                printf("read only %d blocks from big", n);
                exit(1);
            }
            break;
        } else if (i != 512) {
            printf("read failed %d\n", i);
            exit(1);
        }
        if (((int*)buf)[0] != n) {
            printf("read content of block %d is %d\n", n, ((int*)buf)[0]);
            exit(1);
        }
        n++;
    }
    close(fd);
    if (unlink("big") < 0) {
        printf("unlink big failed\n");
        exit(1);
    }
    printf("big files ok\n");
}

void createtest(void) {
    int i, fd;

    printf("many creates, followed by unlink test\n");

    name[0] = 'a';
    name[2] = '\0';
    for (i = 0; i < 52; i++) {
        name[1] = '0' + i;
        fd = open(name, O_CREAT | O_RDWR);
        close(fd);
    }
    name[0] = 'a';
    name[2] = '\0';
    for (i = 0; i < 52; i++) {
        name[1] = '0' + i;
        unlink(name);
    }
    printf("many creates, followed by unlink; ok\n");
}

int main(int argc, char* argv[]) {
    printf("usertests starting\n");

    opentest();
    writetest();
    writetestbig();
    createtest();

    mmap_test();
    fork_test();
    printf("mmaptest: all tests succeeded\n");

    exit(0);
}