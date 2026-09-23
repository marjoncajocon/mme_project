/*
** ethread.c - the few threads mme uses: Search and Go to File walk the
** folder on workers while the editor draws
**
** Windows has CreateThread and a CRITICAL_SECTION, the others pthreads;
** that is all this is. A worker never touches the editor: it takes work
** from a list and leaves what it found on another, both under one lock,
** and the main loop picks it up (esearch.c, mme.c). Nothing here waits
** on a condition: a worker with nothing to do sleeps a moment and looks
** again, which is enough for work that reads the disk.
*/

#include "mme.h"

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#endif


struct Mutex {
#ifdef _WIN32
  CRITICAL_SECTION cs;
#else
  pthread_mutex_t m;
#endif
};

struct Thread {
  void (*fn) (void *);
  void *ud;
#ifdef _WIN32
  HANDLE h;
#else
  pthread_t t;
#endif
};


Mutex *mx_new (void) {
  Mutex *m = (Mutex *)xmalloc(sizeof(Mutex));
#ifdef _WIN32
  InitializeCriticalSection(&m->cs);
#else
  pthread_mutex_init(&m->m, NULL);
#endif
  return m;
}


void mx_free (Mutex *m) {
  if (m == NULL) return;
#ifdef _WIN32
  DeleteCriticalSection(&m->cs);
#else
  pthread_mutex_destroy(&m->m);
#endif
  free(m);
}


void mx_lock (Mutex *m) {
#ifdef _WIN32
  EnterCriticalSection(&m->cs);
#else
  pthread_mutex_lock(&m->m);
#endif
}


void mx_unlock (Mutex *m) {
#ifdef _WIN32
  LeaveCriticalSection(&m->cs);
#else
  pthread_mutex_unlock(&m->m);
#endif
}


#ifdef _WIN32
static DWORD WINAPI run_thread (LPVOID ud) {
  Thread *t = (Thread *)ud;
  t->fn(t->ud);
  return 0;
}
#else
static void *run_thread (void *ud) {
  Thread *t = (Thread *)ud;
  t->fn(t->ud);
  return NULL;
}
#endif


/* a worker's stack, asked for by name: the default is 512 KB on macOS,
** and a regular expression that backtracks needs much more than that */
#define TH_STACK	(8u * 1024 * 1024)


/* fn(ud) on a thread of its own; NULL when the system said no */
Thread *th_start (void (*fn) (void *), void *ud) {
  Thread *t = (Thread *)xmalloc(sizeof(Thread));
  t->fn = fn;
  t->ud = ud;
#ifdef _WIN32
  t->h = CreateThread(NULL, TH_STACK, run_thread, t, 0, NULL);
  if (t->h == NULL) {
    free(t);
    return NULL;
  }
#else
  {
    pthread_attr_t a;
    int rc;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, TH_STACK);
    rc = pthread_create(&t->t, &a, run_thread, t);
    pthread_attr_destroy(&a);
    if (rc != 0) {
      free(t);
      return NULL;
    }
  }
#endif
  return t;
}


/* waits for it to end, then frees it */
void th_join (Thread *t) {
  if (t == NULL) return;
#ifdef _WIN32
  WaitForSingleObject(t->h, INFINITE);
  CloseHandle(t->h);
#else
  pthread_join(t->t, NULL);
#endif
  free(t);
}


/* how many workers are worth starting: the cores, 2 at least, 8 at most */
int th_cpus (void) {
  int n = 4;
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  n = (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
  n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
  if (n < 2) n = 2;
  if (n > 8) n = 8;
  return n;
}


void th_nap (int ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
#endif
}
