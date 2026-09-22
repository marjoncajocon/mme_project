/*
** mmc.h - the part of the mmc shell's header that mme shares:
** mutil.c, mos.c and mpath.c, copied from mmc as they are.
*/

#ifndef mmc_h
#define mmc_h

/* feature macros must come before any system header */
#if !defined(_WIN32)
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MMC_NAME	"mmc"
#define MMC_VERSION	"0.2.0"	/* also in mmc.rc and mterm.rc */
#define MMC_AUTHOR	"Marjon Mangindo Cajocon"
/* what $BASH_VERSION says: scripts check it before using bash features */
#define MMC_BASH_COMPAT	"5.2.0(1)-release"

#define MMC_HISTORY_MAX	1000
#define MMC_SOURCE_DEPTH	64
#define MMC_ALIAS_DEPTH	16
#define MMC_FUNC_DEPTH	1000
#define MMC_FDS		64	/* shell file descriptors 0..63; {var}> takes 10 and up */

/* byte that protects the next character from globbing (see mexpand.c) */
#define QMARK	'\001'


/*
** {==================================================================
** mutil.c - memory, strings, buffers
** ===================================================================
*/

typedef struct Buf {
  char *s;
  size_t len, cap;
} Buf;

/* vector of owned strings; v[n] is always NULL once something is pushed */
typedef struct Vec {
  char **v;
  size_t n, cap;
} Vec;

void *xmalloc (size_t n);
void *xrealloc (void *p, size_t n);
char *xstrdup (const char *s);
char *xstrndup (const char *s, size_t n);
char *xstrcat3 (const char *a, const char *b, const char *c);

void buf_init (Buf *b);
void buf_putc (Buf *b, char c);
void buf_putn (Buf *b, const char *s, size_t n);
void buf_puts (Buf *b, const char *s);
void buf_printf (Buf *b, const char *fmt, ...);
char *buf_take (Buf *b);
void buf_free (Buf *b);

void vec_init (Vec *v);
void vec_push (Vec *v, char *s);
void vec_insert (Vec *v, size_t at, char *s);
void vec_sort (Vec *v);
void vec_free (Vec *v);
void vec_copy (Vec *dst, char *const *src, size_t n);

int fd_puts (int fd, const char *s);
int fd_printf (int fd, const char *fmt, ...);

int m_stricmp (const char *a, const char *b);
int m_strnicmp (const char *a, const char *b, size_t n);
int m_fncmp (const char *a, const char *b);	/* file-name compare */
int m_fnncmp (const char *a, const char *b, size_t n);
int m_envcmp (const char *a, const char *b);	/* env-name compare */
size_t utf8_count (const char *s, size_t nbytes);
int utf8_len (const char *s);	/* bytes of the character at s */
int uc_width (unsigned long cp);	/* terminal columns: 0, 1 or 2 */
char *ll_to_str (long long v, char *out);	/* out: >= 24 bytes */
int is_name (const char *s);	/* [A-Za-z_][A-Za-z0-9_]* */
int is_name_n (const char *s, size_t n);
int str_to_ll (const char *s, long long *out);	/* whole string, base 10 */

char *read_file (const char *native, size_t *len);
void crlf_to_lf (char *s, size_t *len);
int mkdir_p (const char *native);

/* }================================================================== */


/*
** {==================================================================
** mos.c - operating system layer
** ===================================================================
*/

typedef intptr_t OsProc;

typedef struct OsStat {
  int exists, is_dir, is_link, is_fifo, is_sock, is_chr, is_blk;
  unsigned mode;	/* permission bits, 0777 style */
  long long size;
  time_t mtime, atime;
  unsigned long long dev, ino;
  long uid, gid;
} OsStat;

void os_init (void);
void os_shutdown (void);
void os_args (int *argc, char ***argv);

char *os_getenv (const char *name);	/* malloc'd, or NULL */
void os_setenv (const char *name, const char *value);	/* NULL unsets */
void os_env_list (Vec *out);	/* "NAME=value" */

char *os_getcwd (void);
int os_chdir (const char *native);
int os_stat (const char *native, OsStat *st);
int os_lstat (const char *native, OsStat *st);
int os_is_exec (const char *native);
int os_access (const char *native, int what);	/* 'r', 'w' or 'x' */
int os_listdir (const char *native, Vec *out);
int os_mkdir (const char *native);
int os_unlink (const char *native);
char *os_realpath (const char *native);	/* malloc'd, or NULL */

#define OS_READ		0
#define OS_WRITE	1
#define OS_APPEND	2
#define OS_RDWR		3
#define OS_EXCL		4	/* create, fail if it exists (noclobber) */
int os_open (const char *native, int mode);
int os_pipe (int fds[2]);
int os_dup (int fd);
void os_close (int fd);
long os_read (int fd, void *buf, size_t n);
long os_write (int fd, const void *buf, size_t n);
int os_wait_readable (int fd, int ms);	/* 1 ready, 0 timeout, -1 error */
int os_fd_valid (int fd);

int os_is_tty (int fd);
int os_tty_raw (int on);
int os_tty_getbyte (void);
void os_tty_fix (void);
int os_term_cols (void);
int os_term_rows (void);

char *os_exe_path (const char *argv0);
char *os_hostname (void);
char *os_username (void);
long os_getpid (void);
long os_getppid (void);
long os_getuid (void);
long os_geteuid (void);
long long os_now_us (void);	/* wall clock, microseconds */
void os_times (double t[4]);	/* user, sys, children user, children sys */
int os_umask (int mask);	/* returns the old one; -1 only asks */
const char *os_type (void);	/* $OSTYPE */
const char *os_machine (void);	/* $MACHTYPE / $HOSTTYPE */

/* child fd k is fds[k] (k < nfds; -1: closed); envp NULL: our environment */
int os_spawn (const char *exe, char **argv, char **envp, const int *fds,
              int nfds, OsProc *proc, long *pid);
int os_wait (OsProc proc);
int os_poll_proc (OsProc proc, int *status);	/* 1 finished, 0 running, 2 stopped */
/* job control (POSIX terminals; Windows: none, but kill -STOP / -CONT work) */
int os_job_control (int interactive);	/* at start: 1 when it is on */
int os_job_active (void);
void os_job_pgid (long pgid);	/* programs started from now on join group pgid (0: a new one, -1: ours) */
void os_tty_give (long pgid);	/* the terminal to group pgid, 0: back to the shell */
int os_wait_fg (OsProc proc, int *stopped);	/* os_wait; *stopped: Ctrl-Z stopped it */
int os_suspend_self (void);
void os_detach (OsProc proc);
int os_kill (long pid, int sig);
int os_exec (const char *exe, char **argv, char **envp);	/* POSIX only */
int os_can_exec_replace (void);

/* Ctrl-C: set by the signal/console handler, read by the shell */
extern volatile int os_interrupted;
void os_catch_signal (int sig, int on);	/* trap: deliver to os_pending */
extern volatile int os_pending[65];
extern int os_pipe_exit;	/* Windows: a write into a pipe nobody reads ends us (SIGPIPE) */

typedef struct OsThread OsThread;
typedef void (*OsThreadFn) (void *arg);
OsThread *os_thread_start (OsThreadFn fn, void *arg);
void os_thread_join (OsThread *t);
/* a pipe a program opens by its path (<( ) and >( )); to_reader: what is
** written into fd comes out there, else what is written there comes out of fd */
typedef struct OsNPipe OsNPipe;
OsNPipe *os_npipe_new (const char *dir, int fd, int to_reader, char **path);	/* takes fd */
void os_npipe_end (OsNPipe *np);

/* }================================================================== */


/*
** {==================================================================
** mpath.c - Linux style paths ("/" is the MMC root, "/d" is drive D:)
** ===================================================================
*/

#ifdef _WIN32
#define MMC_SEP		'\\'
#define MMC_SEPS	"\\"
#else
#define MMC_SEP		'/'
#define MMC_SEPS	"/"
#endif

void path_set_root (const char *native);
const char *path_root (void);
int path_is_sep (int c);
char *path_join (const char *a, const char *b);
char *path_dirname (const char *native);
const char *path_basename (const char *native);
char *path_to_native (const char *p);
char *path_to_display (const char *native);	/* root mapped to "/" */
char *path_to_drive (const char *native);	/* only "X:" -> "/x" */
char *path_arg_to_native (const char *arg);
char *path_env_to_native (const char *val);
void path_list_split (const char *val, Vec *out);
char *path_list_normalize (const char *val);
char *path_tmpdir (void);	/* where temporary files go, native */

/* }================================================================== */

#endif
