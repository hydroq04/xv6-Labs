//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "ioctl.h"
#include "termios.h"


#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart.
// called by printf, and to echo input characters,
// but not from write().
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // input
#define INPUT_BUF 128
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index

  // support for simple RAW mode
  struct termios termios;
} cons;

int is_set(unsigned mask)
{
    return (cons.termios.c_lflag & (mask)) != 0;
}

void
consechoc(int c)
{
  if(cons.termios.c_lflag & ECHO)
    consputc(c);
}

//
// user write()s to the console go here.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  int i;

  for(i = 0; i < n; i++){
    char c;
    if(either_copyin(&c, user_src, src+i, 1) == -1)
      break;
    uartputc(c);
  }

  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dist indicates whether dst is a user
// or kernel address.
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(myproc()->killed){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF];

    if(c == C('D') && cons.termios.c_lflag & ICANON) {  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n' && cons.termios.c_lflag & ICANON){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  if(cons.termios.c_lflag & ICANON){
    switch(c){
    case C('P'):  // Print process list.
      procdump();
      break;
    case C('U'):  // Kill line.
      while(cons.e != cons.w &&
            cons.buf[(cons.e-1) % INPUT_BUF] != '\n'){
        cons.e--;
        consputc(BACKSPACE);
      }
      break;
    case C('H'): // Backspace
    case '\x7f':
      if(cons.e != cons.w){
        cons.e--;
        consputc(BACKSPACE);
      }
      break;
    default:
      break;
    }
  }
  if(c != 0 && cons.e-cons.r < INPUT_BUF){
    c = (c == '\r') ? '\n' : c;

    // echo back to the user.
    if(cons.termios.c_lflag & ECHO)
       consputc(c);

    // store for consumption by consoleread().
    cons.buf[cons.e++ % INPUT_BUF] = c;

    if(c == '\n' || c == C('D') || cons.e == cons.r+INPUT_BUF
      || (cons.termios.c_lflag & ICANON) == 0){
      // wake up consoleread() if a whole line (or end-of-file)
      // has arrived.
      cons.w = cons.e;
      wakeup(&cons.r);
    } else {
      cons.w = cons.e;
      wakeup(&cons.r);
    }
  }
  
  release(&cons.lock);
}

int
consoleioctl(struct inode *ip, int req, void *ttyctl)
{
  struct termios *termios_p = (struct termios *)ttyctl;
  if(req != TCGETA && req != TCSETA)
    return -1;
//  if(argint(2, (void*)&termios_p, sizeof(*termios_p)) < 0)
//    return -1;

  acquire(&cons.lock);
  if(req == TCGETA) {
    //*termios_p = cons.termios;
    if(either_copyout(1, (uint64)termios_p, (void *)&(cons.termios), sizeof(*termios_p)) == -1)
    {
      release(&cons.lock);
      return -1;
    }
  } else { /* TCSETA */
    //cons.termios = *termios_p;
    if(either_copyin(&termios_p, 1, (uint64)&(cons.termios), sizeof(struct termios)) == -1)
    {
      release(&cons.lock);
      return -1;
    }
  }
  release(&cons.lock);
  return 0;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].ioctl = consoleioctl;

  cons.termios.c_lflag = ECHO | ICANON;

  printf("setting console termios %p\n", cons.termios.c_lflag);
  //cons.locking = 1;
}
