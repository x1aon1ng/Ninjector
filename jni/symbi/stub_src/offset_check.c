#include <stddef.h>
#include <stdio.h>
#include "E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/stub_src/stub.h"
int main() {
  printf("sizeof(TStub)=%zu\n", sizeof(TStub));
  printf("mark=%zu\n", offsetof(TStub, mark));
  printf("original_set_argv0=%zu\n", offsetof(TStub, original_set_argv0));
  printf("slot_addr=%zu\n", offsetof(TStub, slot_addr));
  printf("uid=%zu\n", offsetof(TStub, uid));
  printf("injected=%zu\n", offsetof(TStub, injected));
  printf("package_name=%zu\n", offsetof(TStub, package_name));
  printf("socket_name=%zu\n", offsetof(TStub, socket_name));
  printf("getuid=%zu\n", offsetof(TStub, getuid));
  printf("getpid_fn=%zu\n", offsetof(TStub, getpid_fn));
  printf("getppid_fn=%zu\n", offsetof(TStub, getppid_fn));
  printf("socket_fn=%zu\n", offsetof(TStub, socket_fn));
  printf("connect_fn=%zu\n", offsetof(TStub, connect_fn));
  printf("sendmsg_fn=%zu\n", offsetof(TStub, sendmsg_fn));
  printf("recv_fn=%zu\n", offsetof(TStub, recv_fn));
  printf("close_fn=%zu\n", offsetof(TStub, close_fn));
  printf("raise_fn=%zu\n", offsetof(TStub, raise_fn));
  printf("log_print=%zu\n", offsetof(TStub, log_print));
  return 0;
}
