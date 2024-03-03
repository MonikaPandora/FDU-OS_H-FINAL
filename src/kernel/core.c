#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>
#include <driver/sd.h>
#include <kernel/mem.h>
#include <kernel/paging.h>

bool panic_flag;

NO_RETURN void idle_entry() {
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap { arch_wfi(); }
    }
    set_cpu_off();
    arch_stop_cpu();
}

void trap_return();
static struct proc* first;
extern struct page _pages[];
NO_RETURN void kernel_entry() {
    printk("hello world %d\n", (int)sizeof(struct proc));

    // proc_test();
    // vm_test();
    // sd_init();
    do_rest_init();
    // user_proc_test();
    // sd_test();
    // pgfault_first_test();
    // pgfault_second_test();
    // socket_test();

    // TODO: map init.S to user space and trap_return to run icode
    first = create_proc();
    extern char icode[], eicode[];

    ASSERT(first->pgdir.pt);
    first->ucontext->x[0] = 0;
    first->ucontext->elr = 0x400000;
    first->ucontext->sp = 0x800000;
    first->ucontext->spsr = 0;

    struct section* st = (struct section*)kalloc(sizeof(struct section));
    st->flags = ST_TEXT;
    st->begin = 0x400000;
    st->end = st->begin + (u64)eicode-(u64)icode;
    _insert_into_list(&first->pgdir.section_head, &st->stnode);
    void* p = kalloc_page();
    memcpy(p, (void*)icode, PAGE_SIZE);
    vmmap(&first->pgdir, 0x400000, p, PTE_USER_DATA | PTE_RO);

    start_proc(first, trap_return, 0);
    while(1){
        int code;
        auto pid = wait(&code);
        (void)pid;
    }
    PANIC();
}

NO_INLINE NO_RETURN void _panic(const char *file, int line) {
    printk("=====%s:%d PANIC%d!=====\n", file, line, cpuid());
    panic_flag = true;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}
