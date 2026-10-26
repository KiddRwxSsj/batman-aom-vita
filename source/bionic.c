#include "bionic.h"
#include "util.h"
#include "loader.h"
#include "opensl.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <malloc.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

char bionic_sF[3][84];

static char ctype_tab[257];
static short toupper_tab[257];
const char *bionic_ctype = ctype_tab;
const short *bionic_toupper_tab = toupper_tab;

static void big_init(void);

void bionic_init(void) {
    big_init();
    // BSD style table, index 0 is EOF
    for (int c = 0; c < 256; c++) {
        char f = 0;
        if (c < 128) {
            if (isupper(c)) f |= 0x01;
            if (islower(c)) f |= 0x02;
            if (isdigit(c)) f |= 0x04;
            if (isspace(c)) f |= 0x08;
            if (ispunct(c)) f |= 0x10;
            if (iscntrl(c)) f |= 0x20;
            if (isxdigit(c) && !isdigit(c)) f |= 0x40;
            if (c == ' ') f |= 0x80;
        }
        ctype_tab[c + 1] = f;
        toupper_tab[c + 1] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    toupper_tab[0] = -1;
}

// android absolute paths land under DATA_PATH with their layout intact,
// except the sdcard root which is DATA_PATH itself
const char *fix_path(const char *path, char *buf, size_t size) {
    if (!path || path[0] != '/')
        return path;
    if (strncmp(path, "/sdcard/", 8) == 0)
        snprintf(buf, size, DATA_PATH "%s", path + 8);
    else if (strncmp(path, "/mnt/sdcard/", 12) == 0)
        snprintf(buf, size, DATA_PATH "%s", path + 12);
    else
        snprintf(buf, size, DATA_PATH "%s", path + 1);
    return buf;
}

#define FIX(p) char p##_buf[512]; p = fix_path(p, p##_buf, sizeof(p##_buf))

#define VFD_BASE 0x10000
#define MAX_VFDS 4096

static struct { volatile int used; int64_t pos; } vfds[MAX_VFDS];
static SceUID obb_uid = -1;
static int64_t obb_size;

static inline int is_vfd(int fd) {
    return fd >= VFD_BASE && fd < VFD_BASE + MAX_VFDS && vfds[fd - VFD_BASE].used;
}


static void stat_to_bionic(const struct stat *s, struct bionic_stat *b) {
    memset(b, 0, sizeof(*b));
    b->st_dev = s->st_dev;
    b->__st_ino = s->st_ino;
    b->st_ino = s->st_ino;
    b->st_mode = s->st_mode;
    b->st_nlink = s->st_nlink;
    b->st_size = s->st_size;
    b->st_blksize = 512;
    b->st_blocks = (s->st_size + 511) / 512;
    b->st_atim.tv_sec = s->st_atime;
    b->st_mtim.tv_sec = s->st_mtime;
    b->st_ctim.tv_sec = s->st_ctime;
}

int stat_b(const char *path, struct bionic_stat *buf) {
    FIX(path);
    struct stat st;
    int ret = stat(path, &st);
    if (ret == 0)
        stat_to_bionic(&st, buf);
    VLOG("stat %s -> %d", path, ret);
    return ret;
}

int fstat_b(int fd, struct bionic_stat *buf) {
    if (is_vfd(fd)) {
        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFREG | 0444;
        buf->st_size = obb_size;
        buf->st_blksize = 512;
        buf->st_blocks = (obb_size + 511) / 512;
        return 0;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    struct stat st;
    int ret = fstat(fd, &st);
    if (ret == 0)
        stat_to_bionic(&st, buf);
    return ret;
}

extern volatile int open_fds;

// the engine opens the obb again for every package it loads and keeps those handles while the
// package lives. android allows ~1024 open files, the vita far fewer: a few levels in, opens failed
// with EMFILE and the next load crashed. every open of the obb gets a virtual descriptor instead,
// each with its own position, all reading through one shared kernel handle with sceIoPread
static int vfd_open(void) {
    static volatile int opening;
    if (obb_uid < 0) {
        while (__sync_lock_test_and_set(&opening, 1))
            sceKernelDelayThread(100);
        if (obb_uid < 0) {
            SceUID uid = sceIoOpen(OBB_PATH, SCE_O_RDONLY, 0);
            if (uid >= 0) {
                SceIoStat st;
                sceIoGetstatByFd(uid, &st);
                obb_size = st.st_size;
                obb_uid = uid;
            }
        }
        __sync_lock_release(&opening);
        if (obb_uid < 0) {
            errno = ENOENT;
            return -1;
        }
    }
    for (int i = 0; i < MAX_VFDS; i++) {
        if (!vfds[i].used && __sync_bool_compare_and_swap(&vfds[i].used, 0, 1)) {
            vfds[i].pos = 0;
            return VFD_BASE + i;
        }
    }
    errno = EMFILE;
    return -1;
}

int open_b(const char *path, int flags, ...) {
    FIX(path);
    if (path && (flags & 3) == 0 && !strcmp(path, OBB_PATH)) {
        int ret = vfd_open();
        VLOG("open %s (obb) -> %d", path, ret);
        if (ret < 0)
            LOG("open obb failed, errno %d", errno);
        return ret;
    }
    int mode = 0666;
    if (flags & 0100) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int f = flags & 3;
    if (flags & 0100)   f |= O_CREAT;
    if (flags & 0200)   f |= O_EXCL;
    if (flags & 01000)  f |= O_TRUNC;
    if (flags & 02000)  f |= O_APPEND;
    if (flags & 04000)  f |= O_NONBLOCK;
    int ret = open(path, f, mode);
    VLOG("open %s %x -> %d", path, flags, ret);
    if (ret < 0 && path && *path && !strstr(path, "proc/"))
        LOG("open %s failed, errno %d", path, errno);
    if (ret >= 0)
        __sync_add_and_fetch(&open_fds, 1);
    return ret;
}

// bionic hands back -1/EBADF for a bad descriptor, newlib dereferences a NULL fd entry. the engine
// lseeks the result of a failed open (crashed loading a level)
volatile int open_fds;

int close_b(int fd) {
    if (is_vfd(fd)) {
        vfds[fd - VFD_BASE].used = 0;
        return 0;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    int ret = close(fd);
    if (ret == 0)
        __sync_sub_and_fetch(&open_fds, 1);
    return ret;
}

off_t lseek_b(int fd, off_t off, int whence) {
    if (is_vfd(fd)) {
        int64_t *pos = &vfds[fd - VFD_BASE].pos;
        int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? *pos + off : obb_size + off;
        if (np < 0) {
            errno = EINVAL;
            return -1;
        }
        *pos = np;
        return (off_t)np;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return lseek(fd, off, whence);
}

ssize_t write_b(int fd, const void *buf, size_t n) {
    if (fd < 0 || is_vfd(fd)) {
        errno = EBADF;
        return -1;
    }
    return write(fd, buf, n);
}

ssize_t read_b(int fd, void *buf, size_t n) {
    if (is_vfd(fd)) {
        progress_ticks++;
        int64_t *pos = &vfds[fd - VFD_BASE].pos;
        int ret = sceIoPread(obb_uid, buf, n, *pos);
        if (ret < 0) {
            errno = EIO;
            return -1;
        }
        *pos += ret;
        return ret;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return read(fd, buf, n);
}

typedef struct {
    DIR *dir;
    struct bionic_dirent ent;
} dir_wrap;

DIR *opendir_b(const char *path) {
    // the engine looks for loose files next to its binaries ("../UDKGame/...") before using the obb,
    // walking ".." by name for every obb entry at boot. there are none on the vita, so relative
    // dirs list as empty without touching the filesystem (1500+ scans of app0, one of them hung)
    if (path && path[0] != '/' && !strchr(path, ':')) {
        VLOG("opendir %s -> empty", path);
        return (DIR *)calloc(1, sizeof(dir_wrap));
    }
    FIX(path);
    DIR *d = opendir(path);
    VLOG("opendir %s -> %p", path, d);
    if (!d)
        return NULL;
    dir_wrap *w = calloc(1, sizeof(*w));
    w->dir = d;
    return (DIR *)w;
}

struct bionic_dirent *readdir_b(DIR *d) {
    dir_wrap *w = (dir_wrap *)d;
    if (!w->dir)
        return NULL;
    struct dirent *e = readdir(w->dir);
    if (!e)
        return NULL;
    memset(&w->ent, 0, sizeof(w->ent));
    snprintf(w->ent.d_name, sizeof(w->ent.d_name), "%s", e->d_name);
    w->ent.d_reclen = sizeof(w->ent);
    w->ent.d_type = SCE_S_ISDIR(e->d_stat.st_mode) ? 4 : 8;
    return &w->ent;
}

int closedir_b(DIR *d) {
    dir_wrap *w = (dir_wrap *)d;
    int ret = w->dir ? closedir(w->dir) : 0;
    free(w);
    return ret;
}

int mkdir_b(const char *path, mode_t mode) {
    FIX(path);
    return mkdir(path, mode);
}

int access_b(const char *path, int mode) {
    FIX(path);
    int ret = access(path, mode);
    VLOG("access %s -> %d", path, ret);
    return ret;
}

int rmdir_b(const char *path) { FIX(path); return rmdir(path); }
int unlink_b(const char *path) { FIX(path); return unlink(path); }
int remove_b(const char *path) { FIX(path); return remove(path); }
int chdir_b(const char *path) { FIX(path); return chdir(path); }

int rename_b(const char *from, const char *to) {
    FIX(from);
    FIX(to);
    return rename(from, to);
}

// the template is written back into the caller's buffer, so it can't grow
int mkstemp_b(char *tmpl) {
    char buf[512];
    const char *p = fix_path(tmpl, buf, sizeof(buf));
    if (p == tmpl)
        return mkstemp(tmpl);
    int fd = mkstemp(buf);
    if (fd >= 0)
        memcpy(tmpl + strlen(tmpl) - 6, buf + strlen(buf) - 6, 6);
    return fd;
}

// the .so indexes bionic's __sF with its own sizeof(FILE), so std streams
// show up as pointers into bionic_sF and have to be swapped for newlib's
static FILE *fp(FILE *f) {
    if ((char *)f == bionic_sF[0]) return stdin;
    if ((char *)f == bionic_sF[1]) return stdout;
    if ((char *)f == bionic_sF[2]) return stderr;
    return f;
}

FILE *fopen_b(const char *path, const char *mode) {
    FIX(path);
    FILE *f = fopen(path, mode);
    VLOG("fopen %s %s -> %p", path, mode, f);
    return f;
}

int fclose_b(FILE *f) { return fclose(fp(f)); }
int fflush_b(FILE *f) { return f ? fflush(fp(f)) : 0; }
char *fgets_b(char *s, int n, FILE *f) { return fgets(s, n, fp(f)); }
size_t fread_b(void *p, size_t sz, size_t n, FILE *f) {
    progress_ticks++;
    return fread(p, sz, n, fp(f));
}
int fseek_b(FILE *f, long off, int whence) { return fseek(fp(f), off, whence); }
int fseeko_b(FILE *f, off_t off, int whence) { return fseeko(fp(f), off, whence); }
long ftell_b(FILE *f) { return ftell(fp(f)); }
off_t ftello_b(FILE *f) { return ftello(fp(f)); }
void rewind_b(FILE *f) { rewind(fp(f)); }
void clearerr_b(FILE *f) { clearerr(fp(f)); }

size_t fwrite_b(const void *p, size_t sz, size_t n, FILE *f) {
    f = fp(f);
    if (f == stdout || f == stderr) {
        log_write(p, sz * n);
        return n;
    }
    return fwrite(p, sz, n, f);
}

int swbuf_b(int c, FILE *f) {
    f = fp(f);
    if (f == stdout || f == stderr) {
        char ch = c;
        log_write(&ch, 1);
        return c & 0xff;
    }
    return fputc(c, f);
}

int fprintf_b(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret;
    f = fp(f);
    if (f == stdout || f == stderr) {
        char buf[1024];
        ret = vsnprintf(buf, sizeof(buf), fmt, ap);
        log_printf("%s", buf);
    } else {
        ret = vfprintf(f, fmt, ap);
    }
    va_end(ap);
    return ret;
}

int printf_b(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf("%s", buf);
    return ret;
}

int puts_b(const char *s) {
    log_printf("%s\n", s);
    return 0;
}

// big blocks (the engine's own >= 1 MB allocations) get their own kernel memblocks. those don't
// need one contiguous run of the heap, and heap fragmentation is what killed long sessions in the
// lockdown port. plain user ram while 6 MB stay free (thread stacks come from there), then phycont,
// else the heap
#define BIG_MIN (1 << 20)
#define USER_MARGIN (6 << 20)
#define MAX_BIGS 512

static struct { void *addr; SceUID uid; size_t len, size; } bigs[MAX_BIGS];
static pthread_mutex_t big_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t heap_lo, heap_hi;
volatile unsigned big_count, big_bytes;

static void big_init(void) {
    void *p = malloc(16);
    SceKernelMemBlockInfo info = { sizeof(info) };
    if (sceKernelGetMemBlockInfoByAddr(p, &info) >= 0) {
        heap_lo = (uintptr_t)info.mappedBase;
        heap_hi = heap_lo + info.mappedSize;
    }
    free(p);
}

static void *big_block(SceKernelMemBlockType type, size_t size, size_t len) {
    SceUID uid = sceKernelAllocMemBlock("big", type, size, NULL);
    if (uid < 0)
        return NULL;
    void *p = NULL;
    sceKernelGetMemBlockBase(uid, &p);
    pthread_mutex_lock(&big_lock);
    for (int i = 0; i < MAX_BIGS; i++) {
        if (!bigs[i].addr) {
            bigs[i].addr = p;
            bigs[i].uid = uid;
            bigs[i].len = len;
            bigs[i].size = size;
            big_count++;
            big_bytes += size;
            pthread_mutex_unlock(&big_lock);
            return p;
        }
    }
    pthread_mutex_unlock(&big_lock);
    sceKernelFreeMemBlock(uid);
    return NULL;
}

static void *big_alloc(size_t len) {
    SceKernelFreeMemorySizeInfo info = { sizeof(info) };
    sceKernelGetFreeMemorySize(&info);
    size_t u = (len + 0xfff) & ~0xfff, c = (len + 0xfffff) & ~0xfffff;
    void *p = NULL;
    if ((size_t)info.size_user >= u + USER_MARGIN)
        p = big_block(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, u, len);
    if (!p && (size_t)info.size_phycont >= c)
        p = big_block(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, c, len);
    return p;
}

// big_lock held. heap pointers never scan the table
static int big_find(void *p) {
    if (!p || ((uintptr_t)p >= heap_lo && (uintptr_t)p < heap_hi))
        return -1;
    for (int i = 0; i < MAX_BIGS; i++)
        if (bigs[i].addr == p)
            return i;
    return -1;
}

static void big_free(void *p) {
    if (!p)
        return;
    SceUID uid = -1;
    pthread_mutex_lock(&big_lock);
    int i = big_find(p);
    if (i >= 0) {
        uid = bigs[i].uid;
        big_count--;
        big_bytes -= bigs[i].size;
        bigs[i].addr = NULL;
    }
    pthread_mutex_unlock(&big_lock);
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
    else
        free(p);
}

void *malloc_b(size_t n) {
    void *p = n >= BIG_MIN ? big_alloc(n) : NULL;
    if (!p)
        p = malloc(n);
    return p;
}

void free_b(void *p) {
    big_free(p);
}

void *realloc_b(void *old, size_t n) {
    if (!old)
        return malloc_b(n);
    pthread_mutex_lock(&big_lock);
    int i = big_find(old);
    size_t old_len = i >= 0 ? bigs[i].len : 0;
    pthread_mutex_unlock(&big_lock);
    if (i >= 0 || n >= BIG_MIN) {
        if (i < 0)
            old_len = malloc_usable_size(old);
        void *p = malloc_b(n);
        if (p) {
            sceClibMemcpy(p, old, old_len < n ? old_len : n);
            big_free(old);
        }
        return p;
    }
    return realloc(old, n);
}

extern int _newlib_heap_size_user;

// android reports what the device has, this is the closest the vita gets
int sysinfo_b(struct bionic_sysinfo *info) {
    memset(info, 0, sizeof(*info));
    struct mallinfo mi = mallinfo();
    SceKernelFreeMemorySizeInfo fm = { sizeof(fm) };
    sceKernelGetFreeMemorySize(&fm);
    info->uptime = uptime_ms() / 1000;
    info->totalram = 512 << 20;
    info->freeram = (_newlib_heap_size_user - mi.uordblks) + fm.size_user;
    info->procs = 1;
    info->mem_unit = 1;
    return 0;
}

// 1..34 are the same on newlib and linux, which covers everything UE3 checks
int *errno_b(void) {
    return &errno;
}

void assert2_b(const char *file, int line, const char *func, const char *expr) {
    LOG("assert %s:%d %s: %s", file, line, func, expr);
}

void abort_b(void) {
    LOG("abort() from %p", __builtin_return_address(0));
    sceKernelExitDeleteThread(0);
}

void stack_chk_fail_b(void) {
    LOG("stack smashed in %p", __builtin_return_address(0));
    sceKernelExitDeleteThread(0);
}

int pause_b(void) {
    while (1)
        sceKernelDelayThread(1000000);
    return 0;
}

int gethostname_b(char *name, size_t len) {
    strncpy(name, "psvita", len);
    return 0;
}

uint32_t inet_addr_b(const char *cp) {
    unsigned a, b, c, d;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0xffffffff;
    return a | (b << 8) | (c << 16) | (d << 24);
}

int android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf("[%s] %s\n", tag, buf);
    return 0;
}

void *dlopen_b(const char *name, int flags) {
    LOG("dlopen %s", name ? name : "(null)");
    return (void *)1;
}

void *dlsym_b(void *handle, const char *sym) {
    void *p = opensl_sym(sym);
    if (!p)
        p = vglGetProcAddress(sym);
    LOG("dlsym %s -> %p", sym, p);
    return p;
}

int isfinitef_b(float f) { return isfinite(f); }
int isinff_b(float f) { return isinf(f); }
