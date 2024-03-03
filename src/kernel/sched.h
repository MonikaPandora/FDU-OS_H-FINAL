#pragma once

#include <kernel/proc.h>

void init_schinfo(struct schinfo*);

bool _activate_proc(struct proc*, bool onalert);
#define activate_proc(proc) _activate_proc(proc, false)
#define alert_proc(proc) _activate_proc(proc, true)
WARN_RESULT bool is_zombie(struct proc*);
WARN_RESULT bool is_unused(struct proc*);
void _acquire_sched_lock();
#define lock_for_sched(checker) (checker_begin_ctx(checker), _acquire_sched_lock())
void _sched(enum procstate new_state);
// MUST call lock_for_sched() before sched() !!!
#define sched(checker, new_state) (checker_end_ctx(checker), _sched(new_state))
#define yield() (_acquire_sched_lock(), _sched(RUNNABLE))

WARN_RESULT struct proc* thisproc();
