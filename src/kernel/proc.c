#include <common/list.h>
#include <common/string.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/paging.h>

#define NPAGE_FORPID 1

struct proc root_proc;

void kernel_entry();
// void proc_entry();
u64 proc_entry(void(*entry)(u64), u64 arg);

static SpinLock plock;

static char pid_map[PAGE_SIZE * NPAGE_FORPID];
static SpinLock pid_lock;

int _alloc_pid(){
    _acquire_spinlock(&pid_lock);
    int i;
    int pid = 0;
    for(i = 0; i < PAGE_SIZE * NPAGE_FORPID; i++){
        if(pid_map[i] != 0xff){
            int mask = 0x1;
            while(pid_map[i] & mask){
                pid++;
                mask <<= 1;
            }
            pid_map[i] |= mask;
            break;
        }
        else pid += 8; 
    }
    _release_spinlock(&pid_lock);
    if(i == PAGE_SIZE * NPAGE_FORPID)return -2;
    else return pid;
}

bool _free_pid(int pid){
    if(pid >= 8 * PAGE_SIZE * NPAGE_FORPID)return false;
    _acquire_spinlock(&pid_lock);
    if(pid_map[pid / 8] & (0x1 << (pid % 8))){
        pid_map[pid / 8] ^= (0x1 << (pid % 8));
    }
    _release_spinlock(&pid_lock);
    return true;
}

define_early_init(plock)
{
    init_spinlock(&plock);
}

void set_parent_to_this(struct proc *proc) {
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL
    ASSERT(proc->parent == NULL);
    _acquire_spinlock(&plock);
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    _release_spinlock(&plock);
}

NO_RETURN void exit(int code) {
    // TODO
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there
    // is zombie
    // 4. notify the parent
    // 5. sched(ZOMBIE)
    // NOTE: be careful of concurrency
    auto this = thisproc();
    this->exitcode = code;
    _acquire_spinlock(&plock);
    while(!_empty_list(&this->children)){
        ListNode* p = this->children.next;
        auto proc = container_of(p, struct proc, ptnode);
        _detach_from_list(p);
        proc->parent = &root_proc;
        bool notify = is_zombie(proc);
        _insert_into_list(root_proc.children.prev, p);
        if(notify)post_sem(&root_proc.childexit);
    }
    _release_spinlock(&plock);
    free_pgdir(&this->pgdir);
    _decrement_rc(&this->cwd->rc);
    kfree_page(this->kstack);

    for(int i = 0; i < NOFILE; i++){
        if(this->oftable.files[i]){
            file_close(this->oftable.files[i]);
            this->oftable.files[i] = 0;
        }
    }

    setup_checker(0);
    lock_for_sched(0);
    sched(0, ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}

int wait(int *exitcode) {
    // TODO
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency
    int id = 0;
    auto this = thisproc();
    if(_empty_list(&this->children)){
        return -1;
    }
    bool res;
    res = wait_sem(&this->childexit);
    if(res){
        _acquire_spinlock(&plock);
        _for_in_list(p, &this->children){
            if(p == &this->children)continue;
            struct proc* child = container_of(p, struct proc, ptnode);
            if(is_zombie(child)){
                *exitcode = child->exitcode;
                id = child->pid;
                _detach_from_list(p);
                kfree(child);
                _release_spinlock(&plock);
                _free_pid(id);      // free pid here
                _acquire_spinlock(&plock);
                break;
            }
        }
        _release_spinlock(&plock);
        return id;
    }
    else {
        // alerted when waiting for childexit
        // transfer children to root_proc
        _acquire_spinlock(&plock);
        ListNode* last = _detach_from_list(&this->children);
        _insert_into_list(&root_proc.children, last);
        _release_spinlock(&plock);
        return -1;
    }
}

struct proc* _find_by_pid(struct proc* root, int pid){
    if(root->pid == pid && !is_unused(root))return root;
    struct proc* ret = NULL;
    _for_in_list(p, &root->children){
        if(p == &root->children)continue;
        ret = _find_by_pid(container_of(p, struct proc, ptnode), pid);
        if(ret)break;
    }
    return ret;
}

int kill(int pid) {
    // TODO
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).
    struct proc* p;
    _acquire_spinlock(&plock);
    p = _find_by_pid(&root_proc, pid);
    if(p == NULL){
        _release_spinlock(&plock);
        return -1;
    }
    p->killed = true;
    _release_spinlock(&plock);
    alert_proc(p);
    return 0;
}

int start_proc(struct proc *p, void (*entry)(u64), u64 arg) {
    // TODO
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency
    if(p->parent == NULL){
        _acquire_spinlock(&plock);
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
        _release_spinlock(&plock);
    }
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;
    int id = p->pid;
    activate_proc(p);
    return id;
}

void init_proc(struct proc *p) {
    // TODO
    // setup the struct proc with kstack and pid allocated
    // NOTE: be careful of concurrency
    memset(p, 0, sizeof(struct proc));
    auto pid = _alloc_pid();
    _acquire_spinlock(&plock);
    p->pid = pid;
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_pgdir(&p->pgdir);
    p->kstack = kalloc_page();
    init_schinfo(&p->schinfo);
    p->kcontext = (KernelContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(KernelContext) - sizeof(UserContext));
    p->ucontext = (UserContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
    p->cwd = inodes.share(inodes.root);
    _release_spinlock(&plock);
}

struct proc *create_proc() {
    struct proc *p = kalloc(sizeof(struct proc));
    init_proc(p);
    return p;
}

define_init(root_proc) {
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();
struct proc* sh;  // for debugging
int fork() { /* TODO: Your code here. */
    auto this = thisproc();
    auto new = create_proc();
    if(!sh)sh = new;

    _acquire_spinlock(&plock);
    new->parent = this;
    _insert_into_list(&this->children, &new->ptnode);
    _release_spinlock(&plock);

    memcpy((void*)new->ucontext, (void*)this->ucontext, sizeof(UserContext));
    new->ucontext->x[0] = 0;

    _acquire_spinlock(&this->pgdir.lock);
    _for_in_list(p, &this->pgdir.section_head){
        if(p != &this->pgdir.section_head){
            auto st = container_of(p, struct section, stnode);
            auto new_st = (struct section*)kalloc(sizeof(struct section));
            memset(new_st, 0, sizeof(struct section));
            if(new_st == NULL){
                ASSERT(kill(new->pid) != -1);
                break;
            }
            new_st->begin = st->begin;
            new_st->end = st->end;
            new_st->flags = st->flags;
            if(st->fp){
                new_st->fp = file_dup(st->fp);
                new_st->offset = st->offset;
                new_st->length = st->length;
            }
            _insert_into_list(new->pgdir.section_head.prev, &new_st->stnode);

            for(auto va = PAGE_BASE(st->begin); va < st->end; va += PAGE_SIZE){
                auto pte = get_pte(&this->pgdir, va, false);
                if(pte && (*pte & PTE_VALID)){
                    *pte |= PTE_RO;
                    vmmap(&new->pgdir, va, (void*)P2K(PTE_ADDRESS(*pte)), PTE_FLAGS(*pte));
                    kshare_page(P2K(PTE_ADDRESS(*pte)));
                    // copyout(&new->pgdir, (void*)va, (void*)P2K(PTE_ADDRESS(*pte)), PAGE_SIZE);
                    // auto new_pte = get_pte(&new->pgdir, va, false);
                    // *new_pte |= PTE_USER_DATA | PTE_RW;
                }
            }
        }
    }
    _release_spinlock(&this->pgdir.lock);

    memset((void*)&new->oftable, 0, sizeof(struct oftable));
    if(new->cwd != this->cwd){
        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.put(&ctx, new->cwd);
        bcache.end_op(&ctx);
        new->cwd = inodes.share(this->cwd);
    }

    for(auto i = 0; i < NOFILE; i++){
        if(this->oftable.files[i] && this->oftable.files[i]->type != FD_SOCKET){
            new->oftable.files[i] = file_dup(this->oftable.files[i]);
        }
        else break;
    }

    start_proc(new, trap_return, 0);

    return new->pid;
}