#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <driver/clock.h>

#define NLEVEL 3
#define TIME_TO_LEVEL_UP_MS 1000
#define TIME_SLICE_LEN 5

extern bool panic_flag;

extern void swtch(KernelContext* new_ctx, KernelContext** old_ctx);

static SpinLock mlfq_lock;
// static ListNode rq;
static ListNode mlfq[NLEVEL];
static u64 time_slice[NLEVEL];
bool level_up_timer_set = false;
struct timer level_up_timer;
struct timer time_slice_timers[NCPU];

void _do_level_up(){
    for(int i = 1; i < NLEVEL; i++){
        if(!_empty_list(&mlfq[i])){
            ListNode* first = _detach_from_list(&mlfq[i]);
            first = first->next;
            _for_in_list(p, first){
                auto proc = container_of(p, struct proc, schinfo.rq);
                proc->schinfo.level = 0;
            }
            _merge_list(mlfq[0].prev, first);
        }
    }
}

static void level_up_timer_handler(struct timer* timer){
    timer->data++;
    _acquire_spinlock(&mlfq_lock);
    _do_level_up();
    _release_spinlock(&mlfq_lock);
}

define_init(level_up_timer){
    level_up_timer.triggered = true;
    level_up_timer.elapse = TIME_TO_LEVEL_UP_MS;
    level_up_timer.handler = level_up_timer_handler;
}

static void time_slice_finished(struct timer* timer){
    timer->data++;
    thisproc()->schinfo.left_time_slices--;
    _acquire_sched_lock();
    _sched(RUNNABLE);
}

define_init(time_slice_timers){
    for(int i = 0; i < NCPU; i++){
        time_slice_timers[i].triggered = true;
        time_slice_timers[i].elapse = TIME_SLICE_LEN;
        time_slice_timers[i].handler = time_slice_finished;
    }
}

define_early_init(mlfq)
{
    init_spinlock(&mlfq_lock);
    init_list_node(&mlfq[0]);
    for(int i = 0; i < NLEVEL; i++){
        init_list_node(&mlfq[i]);
        time_slice[i] = 5 * (i + 1);
    }    
}

define_init(sched)
{
    for(int i = 0; i < NCPU; ++i){
        struct proc* p = kalloc(sizeof(struct proc));
        p->idle = 1;
        p->state = RUNNING;
        cpus[i].sched.thisproc = cpus[i].sched.idle = p;
    }
}

struct proc* thisproc()
{
    // TODO: return the current process
    return cpus[cpuid()].sched.thisproc;
}

void init_schinfo(struct schinfo* p)
{
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p->rq);
    p->level = 0;
    p->left_time_slices = time_slice[0];
}

void _acquire_sched_lock()
{
    // TODO: acquire the sched_lock if need
    _acquire_spinlock(&mlfq_lock);
}

void _release_sched_lock()
{
    // TODO: release the sched_lock if need
    _release_spinlock(&mlfq_lock);
}

bool is_zombie(struct proc* p)
{
    bool r;
    _acquire_sched_lock();
    r = p->state == ZOMBIE;
    _release_sched_lock();
    return r;
}

bool is_unused(struct proc* p)
{
    bool r;
    _acquire_sched_lock();
    r = p->state == UNUSED;
    _release_sched_lock();
    return r;
}

bool _activate_proc(struct proc* p, bool onalert)
{
    // TODO
    // if the proc->state is RUNNING/RUNNABLE, do nothing and return false
    // if the proc->state is SLEEPING/UNUSED, set the process state to RUNNABLE, add it to the sched queue, and return true
    // if the proc->state is DEEPSLEEPING, do nothing if onalert or activate it if else, and return the corresponding value.
    _acquire_sched_lock();
    if(p->state == RUNNING || p->state == RUNNABLE || p->state == ZOMBIE){
        _release_sched_lock();
        return false;
    }
    else if(p->state == SLEEPING || p->state == UNUSED){
        p->state = RUNNABLE;
        _insert_into_list(mlfq[p->schinfo.level].prev, &p->schinfo.rq);
    }
    else if (p->state == DEEPSLEEPING){
        if(!onalert){
            p->state = RUNNABLE;
            _insert_into_list(mlfq[p->schinfo.level].prev, &p->schinfo.rq);
        }
        else{
            _release_sched_lock();
            return false;
        }
        
    }
    
    _release_sched_lock();
    return true;
}

static void update_this_state(enum procstate new_state)
{
    // TODO: if using simple_sched, you should implement this routinue
    // update the state of current process to new_state, and remove it from the sched queue if new_state=SLEEPING/ZOMBIE
    if(new_state == RUNNING || new_state == UNUSED)PANIC();
    auto this = thisproc();
    this->state = new_state;
    if(!this->idle)_detach_from_list(&this->schinfo.rq);
    if(this->schinfo.left_time_slices == 0){
        if(this->schinfo.level < NLEVEL - 1)this->schinfo.level++;
        this->schinfo.left_time_slices = time_slice[this->schinfo.level];
    }
    if(new_state == RUNNABLE && !this->idle){
        _insert_into_list(mlfq[this->schinfo.level].prev, &this->schinfo.rq);
    }
    if(new_state == ZOMBIE){    // notify the parent proc here
        _release_sched_lock();
        post_sem(&this->parent->childexit);
        _acquire_sched_lock();
    }
}

static struct proc* pick_next()
{
    // TODO: if using simple_sched, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process
    for(int i = 0; i < NLEVEL; i++){
        if(_empty_list(&mlfq[i]))continue;
        _for_in_list(p, &mlfq[i]){
            if(p == &mlfq[i])continue;
            auto proc = container_of(p, struct proc, schinfo.rq);
            if(proc->state == RUNNABLE)return proc;
        }
    }
    return cpus[cpuid()].sched.idle;
}

static void update_this_proc(struct proc* p)
{
    // TODO: if using simple_sched, you should implement this routinue
    // update thisproc to the choosen process, and reset the clock interrupt if need
    cpus[cpuid()].sched.thisproc = p;
}

static int do_level_cpu;
// A simple scheduler.
// You are allowed to replace it with whatever you like.
static void simple_sched(enum procstate new_state)
{
    // set level_up_timer
    if(level_up_timer.triggered){
        if(do_level_cpu == cpuid())set_cpu_timer(&level_up_timer);
        do_level_cpu = (do_level_cpu + 1) % NCPU;
    }
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    if(this->killed && new_state != ZOMBIE){
        _release_sched_lock();
        return;
    }
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this)
    {
        swtch(next->kcontext, &this->kcontext);
    }
    _release_sched_lock();
    if(thisproc()->pgdir.pt)attach_pgdir(&thisproc()->pgdir);
    if(time_slice_timers[cpuid()].triggered)set_cpu_timer(&time_slice_timers[cpuid()]);
}

__attribute__((weak, alias("simple_sched"))) void _sched(enum procstate new_state);

u64 proc_entry(void(*entry)(u64), u64 arg)
{
    _release_sched_lock();
    if(thisproc()->pgdir.pt)attach_pgdir(&thisproc()->pgdir);
    set_return_addr(entry);
    if(time_slice_timers[cpuid()].triggered)set_cpu_timer(&time_slice_timers[cpuid()]);
    return arg;
}

