#pragma once

#define RAND_MAX 32768

void pgfault_first_test();
void pgfault_second_test();
void alloc_test();
void rbtree_test();
void proc_test();
void ipc_test();
void vm_test();
void user_proc_test();
unsigned rand();
void srand(unsigned seed);
void socket_test();