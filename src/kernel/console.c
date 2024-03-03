#include<kernel/console.h>
#include<kernel/init.h>
#include<aarch64/intrinsic.h>
#include<kernel/sched.h>
#include<driver/uart.h>
#include<driver/interrupt.h>
#define INPUT_BUF 128
struct {
    char buf[INPUT_BUF];
    usize r;  // Read index
    usize w;  // Write index
    usize e;  // Edit index
    SpinLock lock;
    Semaphore readable;
} input;
#define C(x)      ((x) - '@')  // Control-x

void console_interrupt_handler(){
    console_intr(uart_get_char);
}

define_rest_init(console){
    init_spinlock(&input.lock);
    init_sem(&input.readable, 0);
    set_interrupt_handler(IRQ_AUX, console_interrupt_handler);
}

isize console_write(Inode *ip, char *buf, isize n) {
    // TODO
    if(ip){}
    _acquire_spinlock(&input.lock);
    for(int i = 0; i < n; i++){
        uart_put_char(buf[i]);
    }
    _release_spinlock(&input.lock);
    return n;
}

isize console_read(Inode *ip, char *dst, isize n) {
    // TODO
    if(ip){}
    isize i = n;
    _acquire_spinlock(&input.lock);
    while(i){
        if(input.r == input.w){
            _release_spinlock(&input.lock);
            if(wait_sem(&input.readable) == false){
                return -1;
            }
            _acquire_spinlock(&input.lock);
        }
        input.r = (input.r + 1) % INPUT_BUF;
        if(input.buf[input.r] == C('D')){
            if(i < n){
                input.r = (input.r - 1) % INPUT_BUF;
            }
            break;
        }
        *(dst++) = input.buf[input.r];
        i--;
        if(input.buf[input.r] == '\n')break;
    }
    _release_spinlock(&input.lock);
    return n - i;
}

void console_intr(char (*getc)()) {
    // TODO
    char c = getc();
    _acquire_spinlock(&input.lock);
    if(c == '\r')c = '\n';
    if(c == '\x7f'){
        if(input.e != input.w){
            input.e = (input.e - 1) % INPUT_BUF;
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
    }
    else if(c == C('U')){
        while(input.e != input.w && input.buf[(input.e - 1) % INPUT_BUF] != '\n'){
            input.e = (input.e - 1) % INPUT_BUF;
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
    }
    else{
        if((input.e + 1) % INPUT_BUF == input.r){
            _release_spinlock(&input.lock);
            return;
        }
        input.e = (input.e + 1) % INPUT_BUF;
        input.buf[input.e] = c;
        uart_put_char(c);
        if(c == '\n' || c == C('D')){
            input.w = input.e;
            post_sem(&input.readable);
        }
    }
    _release_spinlock(&input.lock);
}
