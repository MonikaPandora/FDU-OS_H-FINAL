#include <common/sem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/pt.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

void *syscall_table[NR_SYSCALL];

void syscall_entry(UserContext *context) {
    // TODO
    // Invoke syscall_table[id] with args and set the return value.
    // id is stored in x8. args are stored in x0-x5. return value is stored in x0.
    u64 id = context->x[8], ret = 0;
    if (id < NR_SYSCALL)
    {
        ret = (*((u64(*)())syscall_table[id]))(context->x[0], context->x[1],
                                                context->x[2], context->x[3], 
                                                context->x[4], context->x[5]);
        // switch (id)
        // {
        // case SYS_myreport:
        //     ret = (*((u64(*)(u64))syscall_table[id]))(context->x[0]);
        //     break;
        
        // default:
        //     break;
        // }
    }
    context->x[0] = ret;
}

// check if the virtual address [start,start+size) is READABLE by the current
// user process
bool user_readable(const void *start, usize size) {
    // TODO
    bool ret = false;
	_for_in_list(node, &thisproc()->pgdir.section_head){
		if(node == &thisproc()->pgdir.section_head)continue;
		auto st = container_of(node, struct section, stnode);
		if(st->begin <= (u64)start && ((u64)start + size) <= st->end){
			ret = true;
            break;
		}
	}
    return ret;
}

// check if the virtual address [start,start+size) is READABLE & WRITEABLE by
// the current user process
bool user_writeable(const void *start, usize size) {
    // TODO
    bool ret = false;
	_for_in_list(node, &thisproc()->pgdir.section_head){
		if(node == &thisproc()->pgdir.section_head)continue;
		auto st = container_of(node, struct section, stnode);
        if(st->flags == ST_TEXT)continue;
		if(st->begin <= (u64)start && ((u64)start + size) <= st->end){
			ret = true;
            break;
		}
	}
    return ret;
}

// get the length of a string including tailing '\0' in the memory space of
// current user process return 0 if the length exceeds maxlen or the string is
// not readable by the current user process
usize user_strlen(const char *str, usize maxlen) {
    for (usize i = 0; i < maxlen; i++) {
        if (user_readable(&str[i], 1)) {
            if (str[i] == 0)
                return i + 1;
        } else
            return 0;
    }
    return 0;
}