#ifndef BIONIC_H
#define BIONIC_H

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>

// arm32 bionic layouts
struct bionic_stat {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    uint64_t st_ino;
};

struct bionic_dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

struct bionic_sysinfo {
    int32_t  uptime;
    uint32_t loads[3];
    uint32_t totalram;
    uint32_t freeram;
    uint32_t sharedram;
    uint32_t bufferram;
    uint32_t totalswap;
    uint32_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t totalhigh;
    uint32_t freehigh;
    uint32_t mem_unit;
    char     _f[8];
};

extern char bionic_sF[3][84];
extern const char *bionic_ctype;
extern const short *bionic_toupper_tab;

void bionic_init(void);
const char *fix_path(const char *path, char *buf, size_t size);

int stat_b(const char *path, struct bionic_stat *buf);
int fstat_b(int fd, struct bionic_stat *buf);
int open_b(const char *path, int flags, ...);
int close_b(int fd);
off_t lseek_b(int fd, off_t off, int whence);
ssize_t read_b(int fd, void *buf, size_t n);
ssize_t write_b(int fd, const void *buf, size_t n);
extern volatile int open_fds;
DIR *opendir_b(const char *path);
struct bionic_dirent *readdir_b(DIR *d);
int closedir_b(DIR *d);
int mkdir_b(const char *path, mode_t mode);
int access_b(const char *path, int mode);
int rmdir_b(const char *path);
int unlink_b(const char *path);
int remove_b(const char *path);
int rename_b(const char *from, const char *to);
int chdir_b(const char *path);
int mkstemp_b(char *tmpl);

FILE *fopen_b(const char *path, const char *mode);
int fclose_b(FILE *f);
int fflush_b(FILE *f);
char *fgets_b(char *s, int n, FILE *f);
int fprintf_b(FILE *f, const char *fmt, ...);
size_t fread_b(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite_b(const void *p, size_t sz, size_t n, FILE *f);
int fseek_b(FILE *f, long off, int whence);
int fseeko_b(FILE *f, off_t off, int whence);
long ftell_b(FILE *f);
off_t ftello_b(FILE *f);
void rewind_b(FILE *f);
void clearerr_b(FILE *f);
int swbuf_b(int c, FILE *f);
int printf_b(const char *fmt, ...);
int puts_b(const char *s);

void *malloc_b(size_t n);
void free_b(void *p);
void *realloc_b(void *old, size_t n);
extern volatile unsigned big_count, big_bytes;

int sysinfo_b(struct bionic_sysinfo *info);
int *errno_b(void);
void assert2_b(const char *file, int line, const char *func, const char *expr);
void abort_b(void);
void stack_chk_fail_b(void);
int pause_b(void);
int gethostname_b(char *name, size_t len);
uint32_t inet_addr_b(const char *cp);
int android_log_print(int prio, const char *tag, const char *fmt, ...);

void *dlopen_b(const char *name, int flags);
void *dlsym_b(void *handle, const char *sym);

int isfinitef_b(float f);
int isinff_b(float f);

int pthread_attr_init_b(void *attr);
int pthread_attr_setstacksize_b(void *attr, size_t size);
int pthread_attr_setdetachstate_b(void *attr, int state);
int pthread_attr_setschedparam_b(void *attr, const void *param);
int pthread_create_b(void *thread, const void *attr, void *(*fn)(void *), void *arg);
int pthread_once_b(int *once, void (*fn)(void));
int pthread_mutex_init_b(int *m, const void *attr);
int pthread_mutex_destroy_b(int *m);
int pthread_mutex_lock_b(int *m);
int pthread_mutex_trylock_b(int *m);
int pthread_mutex_unlock_b(int *m);
int pthread_cond_init_b(int *c, const void *attr);
int pthread_cond_destroy_b(int *c);
int pthread_cond_wait_b(int *c, int *m);
int pthread_cond_timedwait_b(int *c, int *m, const struct timespec *t);
int pthread_cond_signal_b(int *c);
int pthread_cond_broadcast_b(int *c);

#endif
