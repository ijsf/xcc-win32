#if !defined(__WASM)
#include "unistd.h"
#include "_syscall.h"

pid_t wait4(pid_t pid, int* status, int options, struct rusage *usage) {
  int ret;
  __asm("mov %rcx, %r10");  // 4th parameter for syscall is `%r10`. `%r10` is caller save so no need to save/restore
  SYSCALL_RET(__NR_wait4, ret);
  return ret;
}
#endif
