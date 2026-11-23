// LD_PRELOAD diagnostic: logs the caller (object file + offset) of every rand() into $RAND_LOG.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
static int (*real_rand)(void);
static FILE* out;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int rand(void) {
  void* ra = __builtin_return_address(0);
  pthread_mutex_lock(&lock);
  if(!real_rand) real_rand = (int(*)(void))dlsym(RTLD_NEXT,"rand");
  if(!out) { const char* p = getenv("RAND_LOG"); out = p ? fopen(p,"w") : NULL; }
  int r = real_rand();
  if(out) {
    Dl_info info;
    if(dladdr(ra,&info) && info.dli_fname)
      fprintf(out,"%s %lx %ld\n",info.dli_fname,(unsigned long)((char*)ra-(char*)info.dli_fbase),(long)syscall(SYS_gettid));
    else
      fprintf(out,"? %p %ld\n",ra,(long)syscall(SYS_gettid));
  }
  pthread_mutex_unlock(&lock);
  return r;
}
