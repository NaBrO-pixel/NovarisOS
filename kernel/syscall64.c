/* syscall64.c - the MSRs that make SYSCALL work, and what it dispatches to.
 *
 * This is the bring-up half of Milestone 44's item 4, and it is worth
 * being clear about what it is not. Novaris's 32-bit kernel implements
 * *Linux's i386 syscall ABI* - the numbers, the register convention and
 * the structure layouts - and that is precisely what lets real glibc and
 * real Wine run on it unmodified. None of that transfers to x86-64, which
 * has different numbers, a different register convention and different
 * structures. What is here is the mechanism: ring 3 is reachable, a
 * syscall arrives in C, and a value comes back. The ABI on top of it is
 * still to be written. */

#include "syscall64.h"
#include "gdt64.h"
#include "serial64.h"
#include "uspace64.h"
#include "win32_64.h"
#include "sched64.h"
#include "signal64.h"
#include "ramfs64.h"
#include "wmdev64.h"
#include "paging64.h"
#include "kstring.h"
#include "proc64.h"
#include "pipe64.h"
#include "sock64.h"
#include "clock64.h"
#include "elf64.h"
#include "initrd64.h"
#include "pmm64.h"
#include "input64.h"
#include "fb64.h"

#define IA32_EFER   0xC0000080u
#define IA32_STAR   0xC0000081u
#define IA32_LSTAR  0xC0000082u
#define IA32_FMASK  0xC0000084u

#define EFER_SCE    (1ULL << 0)

extern void syscall64_entry(void);

static uint64_t call_count;
static uint64_t last_arg;
static uint64_t exit_code;

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr),
                            "a"((uint32_t)value),
                            "d"((uint32_t)(value >> 32)));
}

void syscall64_init(void) {
    uint64_t star;

    call_count = last_arg = exit_code = 0;

    /* Without SCE the instruction raises #UD rather than doing anything,
     * and the fault looks like a bad opcode rather than a missing bit. */
    write_msr(IA32_EFER, read_msr(IA32_EFER) | EFER_SCE);

    /* STAR[47:32] is the SYSCALL base, STAR[63:48] the SYSRET base. See
     * syscall64.h for how each expands into a CS/SS pair; the values are
     * 0x08 and 0x10 because of how gdt64.c orders its descriptors, and
     * changing that order silently changes which selectors land here. */
    star = ((uint64_t)GDT64_KCODE_SEL << 32)
         | ((uint64_t)GDT64_KDATA_SEL << 48);
    write_msr(IA32_STAR, star);

    write_msr(IA32_LSTAR, (uint64_t)&syscall64_entry);

    /* Bits set here are *cleared* in RFLAGS on entry. IF, so the handler
     * does not run with interrupts on while it is still sorting out its
     * stack; DF, so the string instructions in any C it calls count
     * upwards, which the SysV ABI requires and a ring-3 program is under
     * no obligation to leave true; TF, so a single-stepping debugger in
     * ring 3 does not trap inside the kernel. */
    write_msr(IA32_FMASK, (1ULL << 9) | (1ULL << 10) | (1ULL << 8));
}

static uint64_t bytes_written;

uint64_t syscall64_bytes_written(void) { return bytes_written; }

/* The number of the last call that had no implementation, and how many
 * there were. Discovering what a real program needs is done by running
 * it and reading these, since this kernel has no strace. */
static uint64_t unimpl_count, last_unimpl;

/* How many threads ended through exit(2) without ending the process. */
static uint64_t thread_exits;

/* syscall64.s records the caller's ring-3 stack here on every entry, and
 * it is the only place that value exists - the frame the stub builds
 * describes the registers but not the stack they came off. */
extern uint64_t saved_user_rsp;

/* Open files belong to the process, not to the kernel. Until Milestone
 * 64 this was a static table, which is indistinguishable from correct
 * while there is one program and wrong the moment a fork produces two.
 * 0-2 are never allocated, so stdin/stdout/stderr keep meaning what
 * they mean. */
#define FD_MAX PROC64_FD_MAX
#define fds    (proc64_current()->fds)

/* Gives this process descriptors 0, 1 and 2.
 *
 * They used to be recognised by number and have no table entry at all,
 * which is enough for `write` and for nothing else. Wine turns a Unix
 * descriptor into a Windows handle by *sending it to the wineserver*
 * over SCM_RIGHTS, and the standard streams are among the first it
 * sends - so a descriptor with no entry is one the kernel says it does
 * not have. What that looks like from outside is
 *
 *     wine client error:24: sendmsg: Bad file descriptor
 *
 * naming the socket, which was fine, and saying nothing about the
 * passenger, which was not.
 *
 * stdin is /dev/null rather than nothing: a read of end-of-file is what
 * a program with no input expects, and a descriptor that is simply
 * absent is not. */
void syscall64_open_std(void) {
    int null_node = ramfs64_lookup("/dev/null");
    int con_node  = ramfs64_lookup("/dev/console");

    for (int i = 0; i < 3; i++) {
        int node = (i == 0) ? null_node : con_node;
        if (node < 0) continue;
        fds[i].kind     = FD64_FILE;
        fds[i].node     = node;
        fds[i].pos      = 0;
        fds[i].rx       = -1;
        fds[i].tx       = -1;
        fds[i].sock     = -1;
        fds[i].nonblock = 0;
        fds[i].cloexec  = 0;
        fds[i].used     = 1;
        ramfs64_ref_node(node);
    }
}

void syscall64_reset_files(void) {
    /* Through the macro rather than a local pointer: `fds` expands to a
     * member access, so `p->fds` would expand inside itself. */
    if (!proc64_current()) return;
    for (int i = 0; i < FD_MAX; i++) fds[i].used = 0;
    syscall64_open_std();
}

/* What /proc/self/exe resolves to. Per process, because execve replaces
 * it and a child must not answer with its parent's path. */
void syscall64_set_exe_path(const char* path) {
    proc64_t* p = proc64_current();
    if (!p) return;
    kstrlcpy(p->exe_path, path, PROC64_PATH_MAX);
}

/* Every path a program hands this kernel goes through here first.
 *
 * Until Milestone 68 a relative path was answered -ENOENT, and the
 * comment on openat said why: there was no working directory to measure
 * one against. That is not an obscure gap - `configure`, `ld.so`, make,
 * and Wine all spend most of their path handling relative to where they
 * are, and Wine's very first act on a prefix is to chdir into it.
 *
 * The result is absolute but not textually normalised: "." , ".." and
 * doubled slashes are left for ramfs64's walker, which has to handle
 * them anyway because an absolute path can contain them too. Resolving
 * them here as text would also be wrong - ".." after a symlink is not
 * the parent directory of the link.
 *
 * Returns 0, or a negative errno.
 */
static int64_t abs_path(const char* in, char* out) {
    proc64_t* p = proc64_current();
    uint64_t n = 0, i;

    if (!in) return -14;                               /* -EFAULT */
    if (!in[0]) return -2;                             /* -ENOENT */

    if (in[0] == '/') {
        for (i = 0; in[i]; i++) {
            if (n + 1 >= PROC64_PATH_MAX) return -36;  /* -ENAMETOOLONG */
            out[n++] = in[i];
        }
        out[n] = 0;
        return 0;
    }

    if (!p) return -2;

    for (i = 0; p->cwd[i]; i++) {
        if (n + 1 >= PROC64_PATH_MAX) return -36;
        out[n++] = p->cwd[i];
    }
    /* "/" already ends in one; anything else needs the separator. */
    if (n == 0 || out[n - 1] != '/') {
        if (n + 1 >= PROC64_PATH_MAX) return -36;
        out[n++] = '/';
    }
    for (i = 0; in[i]; i++) {
        if (n + 1 >= PROC64_PATH_MAX) return -36;
        out[n++] = in[i];
    }
    out[n] = 0;
    return 0;
}

/* --- the descriptor layer (Milestone 74) --------------------------- *
 *
 * The address a task blocks on while waiting for a pipe. A third
 * namespace beside PROC64_WAIT_KEY and PROC64_VFORK_KEY, and for the
 * same reason: a reader waiting for bytes and a parent waiting for a
 * child must not wake each other. */
#define PIPE64_WAIT_KEY(p) (0x3000000000000000ULL + (uint64_t)(p))

/* Declared here because a blocking read has to rebuild its own frame to
 * be restarted with, and the definition sits with fork further down. */
static void frame_from_args(const syscall64_args_t* args, uint64_t rax,
                            registers64_t* out);

/* Lowest free descriptor at or above `from`. Linux promises the lowest,
 * and it is not a detail: a program that closes 0 and then opens
 * something expects the new descriptor to *be* 0. dup2 relies on it too. */
static void fds_full(const proc64_fd_t* t);

static int fd_alloc(int from) {
    if (from < 0) from = 0;
    for (int fd = from; fd < FD_MAX; fd++) if (!fds[fd].used) return fd;
    fds_full(fds);
    return -1;
}

/* Says once that a process has run out of descriptors, and what it
 * spent them on.
 *
 * The two tables before this one - pipes and processes - both filled,
 * and in each case the useful question was not "is the number too
 * small" but "what is holding the slots". A descriptor table full of
 * open files is a different bug from one full of client connections:
 * the first is a leak or a working set, the second scales with how many
 * processes are alive and is a number that has to grow with them.
 *
 * Measured here rather than argued, because both look like EMFILE. */
static void fds_full(const proc64_fd_t* t) {
    static int said;
    int files = 0, pipes = 0, socks = 0;
    if (said) return;
    said = 1;
    for (int k = 0; k < FD_MAX; k++) {
        if (!t[k].used) continue;
        if      (t[k].kind == FD64_FILE)   files++;
        else if (t[k].kind == FD64_SOCKET) socks++;
        else                               pipes++;
    }
    serial64_puts("NOVARIS64: [fd] pid ");
    serial64_putdec((uint64_t)proc64_current_pid());
    serial64_puts(" used all ");
    serial64_putdec((uint64_t)FD_MAX);
    serial64_puts(" descriptors - ");
    serial64_putdec((uint64_t)files);
    serial64_puts(" files, ");
    serial64_putdec((uint64_t)pipes);
    serial64_puts(" pipes, ");
    serial64_putdec((uint64_t)socks);
    serial64_puts(" sockets; open now returns -EMFILE\n");
}

static void fd_init_pipe(int fd, int rx, int tx, int nonblock, int cloexec) {
    fds[fd].kind     = FD64_PIPE;
    fds[fd].node     = -1;
    fds[fd].pos      = 0;
    fds[fd].rx       = rx;
    fds[fd].tx       = tx;
    fds[fd].sock     = -1;
    fds[fd].nonblock = nonblock;
    fds[fd].cloexec  = cloexec;
    fds[fd].used     = 1;
    pipe64_ref(rx, 1, 0);
    pipe64_ref(tx, 0, 1);
}

/* A descriptor whose two ends were handed over already referenced - by
 * connect, on behalf of a server, or by accept taking them off the
 * queue. Distinct from fd_init_pipe precisely because it must NOT take
 * a reference: the one connect already took is the one being moved in
 * here, and taking another would leave every accepted connection
 * holding a count nobody ever drops. */
static void fd_adopt_pipe(int fd, int rx, int tx, int sock,
                          int nonblock, int cloexec) {
    fds[fd].kind     = sock >= 0 ? FD64_SOCKET : FD64_PIPE;
    fds[fd].node     = -1;
    fds[fd].pos      = 0;
    fds[fd].rx       = rx;
    fds[fd].tx       = tx;
    fds[fd].sock     = sock;
    fds[fd].nonblock = nonblock;
    fds[fd].cloexec  = cloexec;
    fds[fd].used     = 1;
}

/* Is this descriptor a byte stream - a pipe, or a socket that has been
 * connected? read and write ask, because from their point of view the
 * three things the descriptor layer makes are the same thing. */
static int fd_is_stream(int fd) {
    return fds[fd].kind == FD64_PIPE ||
           (fds[fd].kind == FD64_SOCKET && (fds[fd].rx >= 0 || fds[fd].tx >= 0));
}

/* Closing a descriptor, from close(2) and from dup2 replacing one.
 *
 * The unref is the whole point. A pipe with no readers left makes the
 * next write -EPIPE, which is how a server notices its client is gone,
 * and a pipe with no writers left makes the next read return 0, which is
 * how a client notices the server is. Forget this and both sides wait
 * for each other forever - a hang with nothing in the log. */
/* A descriptor has just been copied into `fd`. Whatever it refers to now
 * has one more name, and everything that counts names has to hear about
 * it - which is the whole list, because getting it wrong in one place
 * looks like a file that vanished or a pipe that never reports end of
 * file. */
static void fd_take_ref(int fd) {
    if (fd < 0 || fd >= FD_MAX || !fds[fd].used) return;
    if (fds[fd].kind == FD64_FILE) {
        ramfs64_ref_node(fds[fd].node);
    } else {
        pipe64_ref(fds[fd].rx, 1, 0);
        pipe64_ref(fds[fd].tx, 0, 1);
    }
}

static void fd_release(int fd) {
    if (fd < 0 || fd >= FD_MAX || !fds[fd].used) return;

    /* A listening socket goes first, and it matters that it does: its
     * queue may still hold connections nobody accepted, whose pipes were
     * referenced by connect on behalf of a server that is now not
     * coming. sock64_destroy gives those back, which is what makes the
     * caller at the other end see end of file rather than wait forever
     * for a reply. */
    if (fds[fd].kind == FD64_SOCKET && fds[fd].sock >= 0) {
        int s = fds[fd].sock;
        sched64_wake(sock64_wait_key(s), SCHED64_MAX_TASKS);
        sock64_destroy(s);
        fds[fd].sock = -1;
    }
    /* A window belongs to the descriptor that made it, so closing the
     * descriptor is what gives its pixels back. Before the node is
     * unreferenced, because the check needs the node to still say what
     * kind of device it was. */
    if (fds[fd].kind == FD64_FILE && fds[fd].node >= 0 &&
        ramfs64_device(fds[fd].node) == RAMFS64_DEV_WM && fds[fd].pos)
        wmdev64_close((int)fds[fd].pos - 1);

    if (fds[fd].kind == FD64_FILE && fds[fd].node >= 0)
        ramfs64_unref_node(fds[fd].node);

    if (fd_is_stream(fd)) {
        int rx = fds[fd].rx, tx = fds[fd].tx;
        pipe64_unref(rx, 1, 0);
        pipe64_unref(tx, 0, 1);
        /* Anyone parked on either pipe is now waiting on a fact that has
         * changed - end of file on one side, a dead reader on the
         * other - and has to be let go to find out. */
        if (rx >= 0) sched64_wake(PIPE64_WAIT_KEY(rx), SCHED64_MAX_TASKS);
        if (tx >= 0) sched64_wake(PIPE64_WAIT_KEY(tx), SCHED64_MAX_TASKS);
    }
    fds[fd].used = 0;
    fds[fd].rx   = -1;
    fds[fd].tx   = -1;
}

/* Leave, and come back to the same call.
 *
 * Used by the two syscalls that have to wait for time rather than for an
 * event - nanosleep and poll. The frame is rewound over the two bytes of
 * `syscall` with the number back in rax, so returning to ring 3
 * re-executes the call.
 *
 * The deadline is kept here, per task, rather than in the caller's
 * registers. Carrying it in a callee-saved register would have been
 * tidier to write and wrong: the entry stub restores r12-r15 from this
 * frame when the call finally returns, so a deadline parked in one of
 * them is handed back to the caller in place of the value it had before
 * the call. Callee-saved means the caller is entitled to it.
 *
 * Then it hands the CPU to another task if there is one. If there is
 * not, it still returns to ring 3 - and that is the point rather than a
 * fallback: interrupts are off inside a syscall, so the timer only
 * advances out there, and a deadline can only be reached by going and
 * coming back.
 *
 * A deadline of 0 means "no timeout", which poll(-1) asks for. */
static uint64_t wait_deadline[SCHED64_MAX_TASKS];
static uint64_t wait_call[SCHED64_MAX_TASKS];
static int      wait_pending[SCHED64_MAX_TASKS];

static int wait_slot(void) {
    int t = sched64_current();
    return (t >= 0 && t < SCHED64_MAX_TASKS) ? t : 0;
}

/* Is this call a restart of one that already began waiting, and if so
 * has its deadline passed? Returns 1 when the caller should give up. */
static int wait_expired(void) {
    int t = wait_slot();
    if (!wait_pending[t]) return 0;
    if (wait_deadline[t] == 0) return 0;               /* no timeout */
    return clock64_ticks() >= wait_deadline[t];
}

static int wait_started(void) { return wait_pending[wait_slot()]; }
static void wait_done(void)   { wait_pending[wait_slot()] = 0; }

static uint64_t wait_restart(const syscall64_args_t* args, uint64_t nr,
                             uint64_t deadline) {
    registers64_t self, next;
    vmspace64_t next_space;
    uint64_t next_fs;
    int t = wait_slot();

    wait_deadline[t] = deadline;
    wait_call[t]     = nr;
    wait_pending[t]  = 1;

    frame_from_args(args, nr, &self);
    self.rip = args->ret_rip - 2;

    if (sched64_yield_current(&self, &next, &next_space, &next_fs)) {
        vmspace64_switch(&next_space);
        write_msr(0xC0000100u, next_fs);
        sched64_resume(&next);                         /* never returns */
    }
    sched64_resume(&self);                             /* never returns */
    return 0;
}

/* Everything this process still had open, given back.
 *
 * Linux closes a process's descriptors when it exits, and until
 * Milestone 76 nothing here did - which was invisible for as long as
 * every test closed what it opened. It stops being invisible the moment
 * a process holds one end of something: a child that exits while
 * holding a socket leaves the pipe believing it still has a reader, so
 * the peer's next read waits for bytes from a process that no longer
 * exists. A leak in the table and a hang at the other end are the same
 * bug seen from two places. */
static void close_all_files(void) {
    if (!proc64_current()) return;

    /* What the process leaves behind on the way out.
     *
     * wineserver learns that a client died by seeing end of file on the
     * socket it shares with it: our poll reports POLLIN when a pipe has
     * no writers left, which is Linux's behaviour. Measured, that is not
     * happening - services.exe exits cleanly at 90% of a run, wineboot
     * blocks three percent later waiting to be told, and the server
     * polls to the end of the run without ever seeing anything ready.
     * So a reference on the write end outlives the process that owned
     * it, and the question is whose.
     *
     * Reported only when something is actually left, so a clean exit
     * stays silent and the line means what it says. */
    for (int fd = 0; fd < FD_MAX; fd++) {
        int tx = -1, inflight = 0;
        if (fds[fd].used && fd_is_stream(fd)) {
            tx = fds[fd].tx;
            if (tx >= 0) inflight = pipe64_inflight(tx);
        }
        fd_release(fd);
        if (tx >= 0 && (pipe64_writers(tx) > 0 || inflight > 0)) {
            serial64_puts("NOVARIS64: [exitfd] pid ");
            serial64_putdec((uint64_t)proc64_current_pid());
            serial64_puts(" fd ");
            serial64_putdec((uint64_t)fd);
            serial64_puts(" tx ");
            serial64_putdec((uint64_t)tx);
            serial64_puts(": writers still ");
            serial64_putdec((uint64_t)pipe64_writers(tx));
            serial64_puts(", ");
            serial64_putdec((uint64_t)inflight);
            serial64_puts(" descriptor batches never received\n");
        }
    }
}

static int64_t do_pipe_write(int fd, const void* buf, uint64_t n) {
    int64_t w;

    if (fds[fd].tx < 0) return -9;                     /* -EBADF: read end */
    w = pipe64_write(fds[fd].tx, buf, n);

    /* -EAGAIN here means the ring is full. A blocking write should park
     * until a reader drains it; this reports it instead, which is
     * honest and is enough for what runs today - Wine's requests are far
     * smaller than 64KB, so a full pipe means the peer has stopped
     * reading rather than that the writer is ahead. Written down rather
     * than hidden: a blocking write that returns EAGAIN is a divergence
     * from Linux, and the differential does not assert it. */
    if (w > 0) sched64_wake(PIPE64_WAIT_KEY(fds[fd].tx), SCHED64_MAX_TASKS);
    return w;
}

/* read(2) on a pipe, including the part that waits.
 *
 * Restarted rather than resumed, the same way wait4 is: the frame is
 * rewound over the two bytes of the `syscall` instruction with the
 * number back in rax, so waking re-runs the call and re-reads the pipe.
 * There is no other way to return a count that was not known when the
 * caller blocked.
 *
 * `nr` is which call to restart *as*, and it is a parameter rather than
 * SYS64_READ because recvmsg reads through here too. Hardcoding read's
 * number worked for as long as read was the only caller and then failed
 * in the least obvious way available: a recvmsg that blocked came back
 * as `read(fd, &msghdr, flags)` - the same arguments, a different call -
 * so the payload landed in the message header instead of the buffer the
 * header pointed at, and Wine reported a protocol version of 4096.
 * sched64.c's own comment on wake_rax says exactly this: a syscall that
 * restarts needs its own number back there, "or the re-executed
 * `syscall` invokes whatever call 0 happens to be". */
static int64_t do_pipe_read(const syscall64_args_t* args, uint64_t nr,
                            int fd, void* buf, uint64_t n) {
    registers64_t self, next;
    vmspace64_t next_space;
    uint64_t next_fs;
    int64_t r;

    if (fds[fd].rx < 0) return -9;                     /* -EBADF: write end */

    r = pipe64_read(fds[fd].rx, buf, n);
    if (r != -11) return r;                            /* data, or EOF */
    if (fds[fd].nonblock) return -11;                  /* -EAGAIN */

    frame_from_args(args, nr, &self);
    self.rip = args->ret_rip - 2;

    if (!sched64_block_current(&self, PIPE64_WAIT_KEY(fds[fd].rx),
                               nr, &next, &next_space, &next_fs)) {
        /* Nothing else can run, so nobody can ever fill this pipe.
         * Reporting it beats a machine that stops. */
        return -11;
    }
    vmspace64_switch(&next_space);
    write_msr(0xC0000100u, next_fs);
    sched64_resume(&next);                             /* never returns */
    return 0;
}

/* Writing to one descriptor, whatever it turns out to be.
 *
 * Factored out because write and writev must not be able to disagree.
 * They did: writev served only stdout and stderr, so the same
 * descriptor accepted a write and refused a writev, which is not a
 * missing feature but two answers to one question.
 *
 * `buf` is a ring-3 pointer dereferenced directly - the kernel runs in
 * the caller's address space - and it is unchecked, which is the honest
 * state of this ABI.
 */
static int64_t fd_write_bytes(uint64_t fd, const char* buf, uint64_t n) {
    /* A descriptor with a table entry is answered from it, whatever its
     * number. The rule below is what is left for a program set up by a
     * layer that never opened the standard three. */
    if (fd < FD_MAX && fds[fd].used) {
        int64_t w;
        if (fd >= FD_MAX || !fds[fd].used) return -9;  /* -EBADF */
        if (fd_is_stream((int)fd)) return do_pipe_write((int)fd, buf, n);
        if (fds[fd].kind == FD64_SOCKET) return -107;  /* -ENOTCONN */
        /* /dev/null takes everything and keeps none of it; the console
         * is the serial port, which is where this kernel's idea of
         * stdout has always gone. */
        if (ramfs64_device(fds[fd].node) == RAMFS64_DEV_NULL)
            return (int64_t)n;
        if (ramfs64_device(fds[fd].node) == RAMFS64_DEV_CON) {
            for (uint64_t i = 0; i < n; i++) serial64_putc(buf[i]);
            bytes_written += n;
            return (int64_t)n;
        }
        w = ramfs64_write(fds[fd].node, fds[fd].pos, buf, n);
        if (w > 0) fds[fd].pos += (uint64_t)w;
        return w;
    }

    /* 0-2 are never allocated, so stdout and stderr are told apart by
     * number rather than by a table entry. */
    if (fd != 1 && fd != 2) return -9;
    for (uint64_t i = 0; i < n; i++) serial64_putc(buf[i]);
    bytes_written += n;
    return (int64_t)n;
}

/* And reading from one. `nr` is the call to restart as if this blocks -
 * see do_pipe_read. */
static int64_t fd_read_bytes(const syscall64_args_t* args, uint64_t nr,
                             uint64_t fd, void* buf, uint64_t n) {
    int64_t r;
    int dev;

    if (fd >= FD_MAX) return -9;
    if (!fds[fd].used) return fd < 3 ? 0 : -9;         /* stdin: end of file */

    if (fd_is_stream((int)fd))
        return do_pipe_read(args, nr, (int)fd, buf, n);
    if (fds[fd].kind == FD64_SOCKET) return -107;      /* -ENOTCONN */

    dev = ramfs64_device(fds[fd].node);
    if (dev == RAMFS64_DEV_NULL) return 0;             /* end of file */

    /* An input device is a stream of events, not a file with a
     * position: there is no offset to advance and nothing to seek to.
     * Whole records only, which is evdev's contract - a reader that got
     * half a struct would resynchronise by guessing. */
    if (dev == RAMFS64_DEV_KBD || dev == RAMFS64_DEV_MOUSE)
        return input64_read(dev == RAMFS64_DEV_KBD ? INPUT64_KBD
                                                   : INPUT64_MOUSE, buf, n);

    r = ramfs64_read(fds[fd].node, fds[fd].pos, buf, n);
    if (r > 0) fds[fd].pos += (uint64_t)r;
    return r;
}

static uint64_t do_open(const char* path, uint64_t flags, uint32_t mode) {
    int node = ramfs64_lookup(path);
    int fd;

    if (node < 0) {
        if (!(flags & O_CREAT)) return (uint64_t)-2;   /* -ENOENT */
        node = ramfs64_create(path, 0);
        if (node < 0) return (uint64_t)-28;            /* -ENOSPC */
        ramfs64_set_mode(node, mode);
    } else if (flags & O_TRUNC) {
        ramfs64_truncate(node);
    }

    for (fd = 3; fd < FD_MAX; fd++) if (!fds[fd].used) break;
    if (fd == FD_MAX) { fds_full(fds); return (uint64_t)-24; }  /* -EMFILE */

    fds[fd].kind     = FD64_FILE;
    fds[fd].node     = node;
    ramfs64_ref_node(node);
    fds[fd].pos      = (flags & O_APPEND) ? ramfs64_size(node) : 0;
    fds[fd].rx       = -1;
    fds[fd].tx       = -1;
    fds[fd].sock     = -1;
    fds[fd].nonblock = (flags & O_NONBLOCK) ? 1 : 0;
    fds[fd].cloexec  = (flags & O_CLOEXEC)  ? 1 : 0;
    fds[fd].used     = 1;
    return (uint64_t)fd;
}

/* st_dev and st_ino, and they are not decoration.
 *
 * ld.so identifies an already-loaded object by the (device, inode) pair
 * it gets back from fstat, so that opening the same library twice by two
 * different paths does not map it twice. Reporting 0/0 for every file
 * makes every file look like the same file: ld.so opens libc, compares
 * it against the main executable it already has, decides they are the
 * same object, and closes it without mapping. What that looks like from
 * outside is "undefined symbol: __libc_start_main" - a symbol error, a
 * long way from the stat that caused it.
 *
 * The node index is a perfectly good inode number: it is unique per file
 * and stable for as long as the file exists. */
static void fill_ids(uint8_t* st, int node) {
    *(uint64_t*)(st + 0) = 1;                          /* st_dev */
    *(uint64_t*)(st + 8) = (uint64_t)node + 1;         /* st_ino */
    *(uint64_t*)(st + 16) = 1;                         /* st_nlink */
}

/* struct stat, x86-64. Only the fields anything here reads are filled;
 * the offsets are the kernel's and must not be tidied.
 *   0 st_dev  8 st_ino  16 st_nlink  24 st_mode  48 st_size  56 st_blksize */
static uint64_t do_stat(const char* path, void* out, int follow) {
    int node = follow ? ramfs64_lookup(path)
                      : ramfs64_lookup_nofollow(path);
    uint8_t* st = (uint8_t*)out;

    if (node < 0) return (uint64_t)-2;                 /* -ENOENT */
    for (int i = 0; i < 144; i++) st[i] = 0;
    fill_ids(st, node);

    /* S_IFDIR, S_IFLNK or S_IFREG. ld.so checks this to decide whether a
     * path it is about to search is a directory at all, and anything
     * walking a tree checks it to decide whether to recurse - an lstat
     * that reported a symlink as an ordinary file would send `cp -r`
     * and Wine's prefix updater into the target instead of copying the
     * link. */
    /* The type bits are the kernel's; the permission bits are whatever
     * the file was created with, which mkdir and open now record rather
     * than this inventing them. A symlink's own mode is not meaningful
     * and Linux reports 0777 for it. */
    /* S_IFSOCK is not decoration either: Wine's client lstats the
     * server's socket and refuses to connect - "'%s/%s' is not a
     * socket" - unless the type bits say so. */
    *(uint32_t*)(st + 24) =
          ramfs64_is_dir(node)                     ? 0040000u | ramfs64_mode(node)
        : ramfs64_is_link(node)                    ? 0120777u
        : ramfs64_device(node) == RAMFS64_DEV_SOCK ? 0140000u | ramfs64_mode(node)
                                                   : 0100000u | ramfs64_mode(node);
    *(uint64_t*)(st + 48) = ramfs64_size(node);
    *(uint64_t*)(st + 56) = 4096;
    return 0;
}

/* readlink(path, buf, bufsiz).
 *
 * Two kinds of answer. /proc/self/exe is synthesised - there is no /proc
 * here, so the path is remembered when the program is loaded - and it
 * earns its place because it is how a program finds out where it was
 * installed: Wine uses it to locate its own lib directory, and without
 * it Wine computes the path to ntdll.so as (null) and stops.
 *
 * The other kind is a real symlink, which this filesystem has had since
 * Milestone 68. A prefix is held together with them: dosdevices/c: is a
 * link to ../drive_c, and Wine reaches drive C by reading it.
 *
 * readlink does not NUL-terminate, and a caller that assumed it did
 * would read past what it was given.
 */
static uint64_t do_readlink(const char* given, char* buf, uint64_t size) {
    char path[PROC64_PATH_MAX];
    proc64_t* me = proc64_current();
    const char* target;
    uint64_t n;
    int node;
    int64_t e;

    if (!buf) return (uint64_t)-14;                    /* -EFAULT */

    if (kstrcmp(given, "/proc/self/exe") == 0) {
        if (!me || !me->exe_path[0]) return (uint64_t)-2;
        target = me->exe_path;
    } else {
        e = abs_path(given, path);
        if (e) return (uint64_t)e;

        node = ramfs64_lookup_nofollow(path);
        if (node < 0) return (uint64_t)-2;             /* -ENOENT */

        target = ramfs64_readlink(node);
        /* Not a symlink. -EINVAL, which is how a caller tells "there is
         * nothing here" from "the thing here is not a link". */
        if (!target) return (uint64_t)-22;
    }

    n = kstrlen(target);
    if (n > size) n = size;
    kmemcpy(buf, target, n);
    return n;
}

static uint64_t forks, execs, vforks;
static uint64_t pipes_made, socketpairs_made, sockets_made;
static uint64_t sendmsgs, recvmsgs, fds_passed, renames, shared_maps;

/* The process a layer started, as opposed to anything it spawned. See
 * syscall64_set_leader. */
static int leader_pid = -1;

/* A wall-clock bound on a run, in timer ticks, or 0. See
 * syscall64_set_run_ticks. */
static uint64_t run_deadline;
static int      run_expired;
static int      leader_exited;

/* syscall64.s */
extern void enter_user_mode64_abort(void) __attribute__((noreturn));
static uint64_t last_fork_frames;

static void frame_from_args(const syscall64_args_t* args, uint64_t rax,
                            registers64_t* out);

/* fork(2). The child is the parent with a different address space, a
 * different pid, and 0 where the parent gets the child's pid.
 *
 * Reached from two syscalls: SYS_fork, and SYS_clone without CLONE_VM,
 * which is what glibc's fork() actually issues.
 *
 * Ordering matters: the address space is the part that can fail, so it
 * happens before anything is committed. A half-built process is worse
 * than a failed fork.
 */
/* Wakes a parent suspended in vfork. Called from execve and from exit -
 * the two ways a vfork child stops being the thing its parent is waiting
 * on - and harmless on a process that was not vforked. */
static void vfork_release(proc64_t* child) {
    if (!child || !child->vfork_parent) return;
    child->vfork_parent = 0;
    sched64_wake(PROC64_VFORK_KEY(child->pid), SCHED64_MAX_TASKS);
}

/* fork, and the two shapes of clone that are really fork.
 *
 * `child_stack` is 0 for a plain fork - the child resumes on its own
 * copy of the parent's stack - and an address for a clone that supplied
 * one, which is how glibc's posix_spawn hands its child a scratch stack.
 *
 * `vfork` suspends the caller until the child execve's or exits. Linux
 * does that by sharing the address space outright, so the child's writes
 * are the parent's. This copies instead - the same choice the 32-bit
 * tree made, and the same honest consequence: a vfork child here can
 * report back through its exit status but not through memory. What it
 * preserves is the part a spawn depends on, which is that the parent
 * does not resume, and therefore does not free the stack the child is
 * standing on, until the child is gone.
 *
 * The copy is cheap here in a way it is not on the 32-bit side, because
 * fork has been copy-on-write since Milestone 69 - a vfork pays for page
 * tables and for whatever the child touches before it execs, which for
 * posix_spawn is a few pages of glibc. */
static uint64_t do_fork_common(const syscall64_args_t* args,
                               uint64_t child_stack, int vfork) {
    registers64_t child;
    int child_pid;
    proc64_t* cp;
    /* What the fork itself cost, in frames. This is the only place the
     * difference between sharing and copying is visible: both produce a
     * correct child, and only one of them charges the resident size of
     * the process for it. */
    uint64_t free_before = pmm64_free_frames();

    child_pid = proc64_fork_from(proc64_current_pid());
    /* The child starts with the parent's handlers, which is what Linux
     * does and what Wine's loader depends on: it installs them once and
     * every process forked from it expects to still have them. */
    if (child_pid > 0) signal64_fork(proc64_current_pid(), child_pid);
    if (child_pid < 0) return (uint64_t)-11;           /* -EAGAIN */
    cp = proc64_get(child_pid);

    if (!vmspace64_create(&cp->space)) {
        proc64_exit(child_pid, 0);
        proc64_reap_child(proc64_current_pid(), 0);
        return (uint64_t)-12;                          /* -ENOMEM */
    }
    /* Shared, not copied (Milestone 69). The eager clone is still there
     * and still correct; it cost the whole resident size of the process
     * and stopped at 16MB, which is not a fork anything the size of
     * Wine can use. */
    if (!vmspace64_clone_cow(vmspace64_current_phys(), &cp->space)) {
        vmspace64_destroy(&cp->space);
        proc64_exit(child_pid, 0);
        proc64_reap_child(proc64_current_pid(), 0);
        return (uint64_t)-12;
    }

    /* The child resumes exactly where the parent is about to, with a 0
     * in rax - which is the whole of how the two tell each other
     * apart. */
    frame_from_args(args, 0, &child);

    /* A clone that supplied a stack runs the child there instead of on
     * its copy of the parent's. The copy is still made - the child needs
     * everything else the parent had - but its first instruction runs on
     * the stack it was given, which is where glibc's clone stub expects
     * to find the function it is about to call. */
    if (child_stack) child.rsp = child_stack;

    /* The child inherits the parent's thread pointer.
     *
     * Passing 0 here - "a task that has never run has no TLS yet" - is
     * right for a brand new program, which will call arch_prctl before
     * it touches TLS, and wrong for a fork, which is a copy of a
     * process that already did. The child comes back from clone inside
     * glibc, whose very next act is to read the thread descriptor at
     * fs:0x10; with FS_BASE at 0 that is a read of linear address 0x10
     * and the child dies on a null dereference that has nothing
     * visibly to do with fork. */
    if (sched64_add_frame_for(&child, &cp->space,
                              read_msr(0xC0000100u), child_pid) < 0) {
        vmspace64_destroy(&cp->space);
        return (uint64_t)-11;
    }
    forks++;
    {
        uint64_t now = pmm64_free_frames();
        last_fork_frames = free_before > now ? free_before - now : 0;
    }

    if (vfork) {
        registers64_t self, next;
        vmspace64_t next_space;
        uint64_t next_fs;

        cp->vfork_parent = proc64_current_pid();
        vforks++;

        /* Parked on the child's key until it execs or exits. Unlike
         * wait4 this is not restarted: the answer is already known, so
         * the frame is built with the child's pid in rax and the parent
         * simply carries on from the instruction after its `syscall`
         * when it is woken.
         *
         * If nothing else is runnable the block fails, and the caller
         * gets that same pid without ever having waited - which cannot
         * happen here, because the child was made runnable above, and
         * which is the right answer anyway. */
        frame_from_args(args, (uint64_t)child_pid, &self);
        /* The third argument is what rax holds when the task is woken,
         * not the syscall number: wait4 passes its own number there
         * because it is *restarted*, and this is not. Passing SYS64_CLONE
         * here made the parent's clone() return 56, and posix_spawn
         * reported 56 as the child's pid - a plausible number that
         * nothing could then wait for. */
        if (sched64_block_current(&self, PROC64_VFORK_KEY(child_pid),
                                  (uint64_t)child_pid, &next, &next_space,
                                  &next_fs)) {
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);
            sched64_resume(&next);                     /* never returns */
        }
    }

    return (uint64_t)child_pid;
}

static uint64_t do_fork(const syscall64_args_t* args) {
    return do_fork_common(args, 0, 0);
}

uint64_t syscall64_forks(void) { return forks; }
uint64_t syscall64_execs(void) { return execs; }
uint64_t syscall64_vforks(void) { return vforks; }
uint64_t syscall64_pipes(void) { return pipes_made; }
uint64_t syscall64_socketpairs(void) { return socketpairs_made; }
uint64_t syscall64_sockets(void) { return sockets_made; }
uint64_t syscall64_sendmsgs(void) { return sendmsgs; }
uint64_t syscall64_recvmsgs(void) { return recvmsgs; }
uint64_t syscall64_fds_passed(void) { return fds_passed; }
uint64_t syscall64_renames(void) { return renames; }
uint64_t syscall64_shared_maps(void) { return shared_maps; }
uint64_t syscall64_last_fork_frames(void) { return last_fork_frames; }

/* The calling thread's complete user state, as a frame it could be
 * resumed from, with `rax` set to what the syscall will return.
 *
 * This is what makes a blocking syscall possible without a per-task
 * kernel stack: a thread that blocks does not leave a half-finished
 * kernel call behind, it leaves *this*, and waking it is resuming it. */
static void frame_from_args(const syscall64_args_t* args, uint64_t rax,
                            registers64_t* out) {
    out->rax = rax;
    out->rbx = args->rbx;
    out->rcx = 0;              /* SYSCALL destroyed it on the way in */
    out->rdx = args->a3;
    out->rsi = args->a2;
    out->rdi = args->a1;
    out->rbp = args->rbp;
    out->r8  = args->a5;
    out->r9  = args->a6;
    out->r10 = args->a4;
    out->r11 = 0;
    out->r12 = args->r12;
    out->r13 = args->r13;
    out->r14 = args->r14;
    out->r15 = args->r15;
    out->int_no   = 0;
    out->err_code = 0;
    out->rip    = args->ret_rip;
    out->cs     = 0x23;
    out->rflags = args->ret_rflags | 0x200;
    out->rsp    = saved_user_rsp;
    out->ss     = 0x1B;
}

uint64_t syscall64_thread_exits(void) { return thread_exits; }

/* Used when the kernel ends a program itself - a fault with no handler -
 * so that the status a test reads is the one the kernel decided on
 * rather than whatever the last program to exit left behind. */
void syscall64_set_exit_code(uint64_t code) { exit_code = code; }

/* Threads that actually slept, and wakeups that actually woke one.
 * Counted because "the futex worked" and "the futex was never contended"
 * look identical from outside, and only the first is worth claiming. */
static uint64_t futex_timed, futex_shared;
static uint64_t futex_waits, futex_wakes;

/* File-backed mappings made. Counted for the same reason the futex
 * counters are: a program can be handed a correct-looking pointer by
 * the anonymous path and never notice the file was not involved. */
static uint64_t file_maps;

uint64_t syscall64_file_maps(void) { return file_maps; }

uint64_t syscall64_futex_waits(void) { return futex_waits; }
uint64_t syscall64_futex_timed(void)  { return futex_timed; }
uint64_t syscall64_futex_shared(void) { return futex_shared; }
uint64_t syscall64_futex_wakes(void) { return futex_wakes; }

uint64_t syscall64_unimplemented(void) { return last_unimpl; }
uint64_t syscall64_unimplemented_count(void) { return unimpl_count; }

/* Off by default: it prints on every call, so leaving it on would bury
 * the program's own output in the transcript the tests match against. */
static int trace;

void syscall64_set_trace(int on) { trace = on; }

/* Which process a layer is actually waiting for.
 *
 * enter_user_mode64 returns when the *last* runnable task exits, which
 * was right while every program this kernel ran was the only one. It
 * stops being right the moment a program starts a daemon: wineboot
 * finishes and the wineserver goes on polling for the next client
 * forever, exactly as it is supposed to, so the run never ends and the
 * layer never gets to report what happened.
 *
 * Naming the leader makes "the thing I started has finished" the end of
 * the run, and leaves whatever it started behind - which is also what a
 * shell does. -1 restores the old behaviour. */
void syscall64_set_leader(int pid) {
    leader_pid = pid;
    /* Only *arming* clears the verdict, for exactly the reason spelled
     * out under set_run_ticks below - and this is the same bug, which
     * was fixed there and left here.
     *
     * The layer disarms before it reports:
     *
     *     syscall64_set_leader(-1);
     *     ...
     *     if (syscall64_leader_exited()) ... else "did not exit"
     *
     * so clearing on disarm made syscall64_leader_exited() answer false
     * every time it was ever asked. "wineboot did not exit" was not a
     * measurement, it was a constant - and Milestone 80 recorded it as
     * a finding, as did several runs of Milestone 81 before the trace
     * showed pid 1, the leader, calling exit_group as its last act. */
    if (pid >= 0) leader_exited = 0;
}

/* Ends a run after `ticks` whether or not anything has finished.
 *
 * A layer that runs a real program cannot assume the program ends. This
 * one runs wineboot, which starts a server that is *supposed* to keep
 * running, and if wineboot itself stops making progress there is nothing
 * to notice it - the machine looks exactly like a healthy idle system.
 * The bound turns "it never finished" into a reported fact rather than a
 * test that hangs, and it is checked at the syscall boundary because
 * that is the only place this kernel is ever in.
 *
 * 0 disables it. */
void syscall64_set_run_ticks(uint64_t ticks) {
    run_deadline = ticks ? clock64_ticks() + ticks : 0;
    /* Only an armed watchdog clears the verdict. Clearing it on disarm
     * as well threw away the answer: the layer disarms before it reports,
     * so a run that had just timed out described itself as having ended
     * on its own. */
    if (ticks) run_expired = 0;
}

int syscall64_run_expired(void) { return run_expired; }
int syscall64_leader_exited(void) { return leader_exited; }

static uint64_t dispatch(syscall64_args_t* args);

/* --- the KUSER_SHARED_DATA canary (Milestone 81) --------------------- */

/* The physical address of KUSER_SHARED_DATA.SystemCall, learned when the
 * page is first mapped shared at 0x7ffe0000. Read through the direct
 * map, so it reports the frame's contents whatever the current process
 * has mapped - including from a process that never mapped it at all. */
static uint64_t usd_byte_phys;
static uint8_t  usd_last;

void syscall64_watch_usd(uint64_t frame0) {
    usd_byte_phys = frame0 + 0x308;
    usd_last = *(volatile uint8_t*)phys64_to_virt(usd_byte_phys);
}

/* Says whether the byte changed, and reports the change once.
 *
 * The byte is written 1 by every process that starts, and every Windows
 * syscall stub tests it. Measured across a prefix run it goes back to 0
 * and stays there, and the frame is neither freed nor reallocated while
 * that happens - so a store is doing it. Checking on both sides of every
 * system call narrows "a store somewhere in the system" down to one
 * call in one process, which is as close as the kernel can get to
 * naming the instruction. */
static void usd_check(const char* when, syscall64_args_t* args) {
    uint8_t now;
    if (!usd_byte_phys) return;
    now = *(volatile uint8_t*)phys64_to_virt(usd_byte_phys);
    if (now == usd_last) return;
    serial64_puts("NOVARIS64: [usd ");
    serial64_puts(when);
    serial64_puts("] ");
    serial64_putdec(usd_last);
    serial64_puts(" -> ");
    serial64_putdec(now);
    serial64_puts(" pid ");
    serial64_putdec((uint64_t)proc64_current_pid());
    serial64_puts(" syscall ");
    serial64_putdec(args->nr);
    serial64_puts(" rip=");
    serial64_puthex(args->ret_rip);
    serial64_putc('\n');
    usd_last = now;
}

/* The wrapper exists only so that the canary sees both sides of the
 * call: the body below has a dozen return points and a store that
 * happens inside any of them must be attributed to that call, not to
 * whichever call happens to run next. */
static uint64_t syscall64_dispatch_inner(syscall64_args_t* args);

uint64_t syscall64_dispatch(syscall64_args_t* args) {
    uint64_t r;

    usd_check("before", args);
    r = syscall64_dispatch_inner(args);
    usd_check("after", args);
    return r;
}

static uint64_t syscall64_dispatch_inner(syscall64_args_t* args) {
    uint64_t r;

    if (run_deadline && clock64_ticks() > run_deadline) {
        run_expired = 1;
        run_deadline = 0;
        serial64_puts("\nNOVARIS64: *** the run reached its time limit\n");
        enter_user_mode64_abort();                     /* never returns */
    }

    if (!trace) return dispatch(args);

    /* A call that is being restarted is not a new call, and printing it
     * again says nothing that the first line did not. It matters here
     * rather than being tidiness: the wineserver's idle loop is a poll
     * with a thirty-second timeout, and a timeout is reached by leaving
     * and coming back - thousands of times. Traced, one idle server
     * produced a hundred megabytes of log saying the same thing.
     *
     * The first entry is traced, and the line is finished by whatever
     * the call eventually returns, so a restarted call still reads as
     * one call with one answer. */
    if (wait_started() && args->nr == wait_call[wait_slot()])
        return dispatch(args);

    /* Which process is asking.
     *
     * Without it a trace of a Wine prefix is four programs' syscalls
     * interleaved on one line-oriented log, and the question that
     * matters - which of them stopped making progress - cannot be asked
     * of it at all. wineboot spawns the wineserver and a rundll32 per
     * wine.inf section, and "the last thing in the log is a poll loop"
     * describes both a healthy idle server and a wedged client. */
    serial64_puts("NOVARIS64: [call pid ");
    serial64_putdec((uint64_t)proc64_current_pid());
    serial64_puts("] ");
    serial64_putdec(args->nr);

    /* Path-taking calls print their path. Without this a trace of a
     * dynamic loader is a wall of pointers, and the whole question -
     * which file could it not find - is the one thing it does not
     * answer. The argument holding the path differs per call. */
    if (args->nr == SYS64_OPEN || args->nr == SYS64_ACCESS ||
        args->nr == SYS64_STAT || args->nr == SYS64_UNLINK ||
        args->nr == SYS64_LSTAT || args->nr == SYS64_CHDIR ||
        args->nr == SYS64_MKDIR || args->nr == SYS64_RMDIR ||
        args->nr == SYS64_READLINK || args->nr == SYS64_EXECVE) {
        serial64_puts(" \"");
        serial64_puts((const char*)args->a1);
        serial64_puts("\"");
    } else if (args->nr == SYS64_OPENAT || args->nr == SYS64_NEWFSTATAT ||
               args->nr == SYS64_READLINKAT || args->nr == SYS64_UNLINKAT ||
               args->nr == SYS64_MKDIRAT) {
        serial64_puts(" \"");
        serial64_puts((const char*)args->a2);
        serial64_puts("\"");
    }

    /* And execve prints its arguments, not just its path.
     *
     * Every Wine process on this machine execs the same file - the
     * loader, /usr/bin/wine - so the path answers nothing about which
     * program a pid is. The Windows program is argv[1] and what it was
     * asked to do is the rest, which is the difference between "pid 5
     * is a Wine process" and "pid 5 is rundll32 running wine.inf's
     * DefaultInstall section". Four arguments is enough for that and
     * short enough not to bury the line. */
    if (args->nr == SYS64_EXECVE && args->a2) {
        const char* const* av = (const char* const*)args->a2;
        for (int i = 0; i < 4 && av[i]; i++) {
            serial64_puts(i ? " " : " [");
            serial64_puts(av[i]);
        }
        if (av[0]) serial64_puts("]");
    }

    serial64_puts("(");
    serial64_puthex(args->a1);
    serial64_puts(", ");
    serial64_puthex(args->a2);
    serial64_puts(", ");
    serial64_puthex(args->a3);
    /* mmap's interesting arguments are the ones past the third: whether
     * it is anonymous, which file, and at what offset. A trace of a
     * loader without them cannot be compared to what the ELF says. */
    if (args->nr == SYS64_MMAP) {
        serial64_puts(" flags=");
        serial64_puthex(args->a4);
        serial64_puts(" fd=");
        serial64_putdec(args->a5);
        serial64_puts(" off=");
        serial64_puthex(args->a6);
    }

    serial64_puts(")");
    r = dispatch(args);
    serial64_puts(" = ");
    serial64_puthex(r);
    serial64_putc('\n');
    return r;
}

/* Called from syscall64_entry with a pointer to the pushed arguments. */
/* execve(2).
 *
 * The hard part is that there is no going back. Once the old address
 * space is gone the caller's stack, code and arguments are gone with
 * it - including the strings execve was passed - so everything needed
 * from the old process is copied out first, and the new space is built
 * before the old one is discarded. A failure after that point cannot
 * return an error to a caller that no longer exists.
 */
/* How much of an argument list execve will carry.
 *
 * It was 8 entries of 128 bytes, and it truncated silently, which is
 * the combination that cost Milestone 81 its five minutes.
 *
 * Wine's loader re-execs itself to pick the right loader for the target
 * machine, and it stops the recursion by telling the next image not to
 * do it again:
 *
 *     static char noexec[] = "WINELOADERNOEXEC=1";
 *     putenv( noexec );
 *     ... preloader_exec( argv );
 *
 * putenv appends. The layer supplies six variables - HOME, USER,
 * WINEPREFIX, WINEDEBUG, WINEDLLPATH, WINEBOOTSTRAPMODE - and Wine adds
 * three of its own: WINELOADERNOEXEC, WINEPRELOADRESERVE and
 * WINESERVERSOCKET. Nine, against a ceiling of eight, and the one that
 * fell off the end was the guard. The child re-execed, was told nothing,
 * re-execed again, and after eight rounds of that gave up with 127 -
 * while wineboot sat waiting for it.
 *
 * 128 entries because a real environment is that size: the same
 * wineboot run on Linux carries 140 variables through execve, and a
 * ceiling below what the host does is a ceiling this kernel will meet
 * again. 512 bytes an entry for the same reason - PROC64_PATH_MAX is
 * 1024, and an argument naming a path was being cut at 128.
 *
 * It costs 128KB of bss, in two static stores, and it is not silent any
 * more: over either limit is -E2BIG, which is what Linux says. */
#define EXECVE_MAX_ARGS 128
#define EXECVE_ARG_MAX  512

/* Is a user pointer safe to read or write for `len` bytes?
 *
 * The kernel reaches into user memory all over this file, and until now
 * it did so on trust. That is survivable while every caller is a test
 * program the same tree wrote, and it stops being survivable the moment
 * a real one passes a value it got from somewhere else: Wine handed
 * nanosleep -38 - an -ENOSYS return used as a struct timespec * - and
 * the read of req->tv_sec faulted in ring 0 and halted the machine.
 * Linux answers -EFAULT and stays up.
 *
 * A page-table walk rather than a range check, because the address is
 * only meaningful in the current space and that is what is mapped. */
static int user_range_ok(uint64_t addr, uint64_t len) {
    uint64_t p, phys;

    if (!addr || !len) return 0;
    /* Nothing user-space owns lives at or above the kernel half, and a
     * sign-extended small negative - which is what a stray errno looks
     * like as a pointer - lands there. */
    if (addr >= 0x0000800000000000ULL) return 0;
    if (addr + len < addr) return 0;                   /* wrapped */
    for (p = addr & ~0xFFFULL; p < addr + len; p += 0x1000)
        if (paging64_translate(p, &phys) != 0) return 0;
    return 1;
}

/* How much an address-space teardown actually gave back.
 *
 * Three separate leaks have been fixed here and the run still reaches
 * zero free frames, so the question is no longer "is something
 * leaking" but "is the reclaim doing anything". A teardown that returns
 * a handful of frames is a teardown that is not working; one that
 * returns thousands while memory still runs out means the memory is
 * held somewhere these paths never touch. The two answers point in
 * opposite directions and cannot be told apart from an out-of-memory
 * count. */
static void reclaim_report(const char* why, uint64_t before) {
    uint64_t after = pmm64_free_frames();
    serial64_puts("NOVARIS64: [reclaim ");
    serial64_puts(why);
    serial64_puts("] pid ");
    serial64_putdec((uint64_t)proc64_current_pid());
    serial64_puts(" gave back ");
    serial64_putdec(after > before ? after - before : 0);
    serial64_puts(" frames, free=");
    serial64_putdec(after);
    serial64_putc('\n');
}

static uint64_t do_execve(const char* path, const char* const* argv,
                          const char* const* envp,
                          const syscall64_args_t* args) {
    /* Copied into the kernel while the old space still exists. */
    static char  kpath[PROC64_PATH_MAX];
    static char  kargv_store[EXECVE_MAX_ARGS][EXECVE_ARG_MAX];
    static char  kenvp_store[EXECVE_MAX_ARGS][EXECVE_ARG_MAX];
    static const char* kargv[EXECVE_MAX_ARGS + 1];
    static const char* kenvp[EXECVE_MAX_ARGS + 1];
    const uint64_t STACK_TOP   = 0x00007FFFFFFF0000ULL;
    const uint64_t STACK_PAGES = 64;
    const uint64_t EXE_BIAS    = 0x0000555555554000ULL;
    const uint64_t INTERP_BASE = 0x00007FFFF7000000ULL;
    static char  kinterp[PROC64_PATH_MAX];
    const void* image;
    uint64_t len, rsp;
    uint64_t exe_bias = 0, interp_base = 0;
    vmspace64_t fresh, old_space;
    elf64_info_t info, interp;
    registers64_t entry;
    proc64_t* p = proc64_current();
    int nargv = 0, nenvp = 0;

    if (!p) return (uint64_t)-1;
    (void)args;

    /* Made absolute against the working directory here, so that
     * execve("./configure") works and so that /proc/self/exe answers
     * with a path that still means something after a chdir. */
    if (abs_path(path, kpath) != 0) return (uint64_t)-2;
    /* Counted before copying, and refused rather than trimmed. A list
     * that does not fit is -E2BIG on Linux; quietly delivering a
     * shorter one is how a loop guard goes missing. */
    for (; argv && argv[nargv]; nargv++) {
        if (nargv >= EXECVE_MAX_ARGS) return (uint64_t)-7;      /* -E2BIG */
        if (kstrlen(argv[nargv]) >= EXECVE_ARG_MAX) return (uint64_t)-7;
        kstrlcpy(kargv_store[nargv], argv[nargv], sizeof(kargv_store[0]));
        kargv[nargv] = kargv_store[nargv];
    }
    kargv[nargv] = 0;
    for (; envp && envp[nenvp]; nenvp++) {
        if (nenvp >= EXECVE_MAX_ARGS) return (uint64_t)-7;      /* -E2BIG */
        if (kstrlen(envp[nenvp]) >= EXECVE_ARG_MAX) return (uint64_t)-7;
        kstrlcpy(kenvp_store[nenvp], envp[nenvp], sizeof(kenvp_store[0]));
        kenvp[nenvp] = kenvp_store[nenvp];
    }
    kenvp[nenvp] = 0;

    /* The path is looked up before anything is torn down, so a missing
     * program is an ordinary -ENOENT rather than a dead process. From
     * the filesystem rather than the initrd, so a program that was
     * written at run time is as executable as one that shipped. */
    {
        int node = ramfs64_lookup(kpath);
        if (node < 0) return (uint64_t)-2;             /* -ENOENT */
        image = ramfs64_data(node);
        len   = ramfs64_size(node);
        if (!image || !len) return (uint64_t)-8;       /* -ENOEXEC */
    }

    if (!vmspace64_create(&fresh)) return (uint64_t)-12;

    /* A dynamically linked program cannot simply be loaded and jumped
     * into (Milestone 71).
     *
     * Everything execve had run until now was static, so entering at
     * e_entry was right. A PIE that names an interpreter asks to be
     * placed somewhere and expects ld.so to be mapped alongside it and
     * entered *instead*; jumping to its own e_entry lands in relocated
     * nonsense. What that looks like is a fault at a single-digit RIP,
     * and it is exactly where Wine's loader died - it re-execs itself
     * as /usr/bin/wine, which is a PIE.
     *
     * e_type is read from the header rather than from a load, because
     * the bias has to be chosen before the load rather than after it.
     * ET_DYN is 3. */
    {
        uint16_t e_type = (uint16_t)((const uint8_t*)image)[16]
                        | (uint16_t)(((const uint8_t*)image)[17] << 8);
        exe_bias = (e_type == 3) ? EXE_BIAS : 0;
    }

    if (elf64_load_at(image, len, &fresh, exe_bias, &info) != ELF64_OK) {
        vmspace64_destroy(&fresh);
        return (uint64_t)-8;                           /* -ENOEXEC */
    }

    if (info.has_interp) {
        const void* iimage;
        uint64_t ilen;
        int inode;

        if (!elf64_interp(image, len, kinterp, sizeof(kinterp))) {
            vmspace64_destroy(&fresh);
            return (uint64_t)-8;
        }
        inode = ramfs64_lookup(kinterp);
        if (inode < 0) {
            /* The interpreter is a file like any other, and a missing
             * one is -ENOENT against the *program*, which is what
             * Linux reports too. */
            vmspace64_destroy(&fresh);
            return (uint64_t)-2;
        }
        iimage = ramfs64_data(inode);
        ilen   = ramfs64_size(inode);
        if (!iimage || !ilen ||
            elf64_load_at(iimage, ilen, &fresh, INTERP_BASE, &interp)
                != ELF64_OK) {
            vmspace64_destroy(&fresh);
            return (uint64_t)-8;
        }
        interp_base = INTERP_BASE;
    }

    for (uint64_t i = 0; i < STACK_PAGES; i++) {
        uint64_t f = pmm64_alloc_frame();
        if (!f || vmspace64_map(&fresh, STACK_TOP - (i + 1) * PAGE64_SIZE,
                                f, PAGE64_PRESENT | PAGE64_WRITE |
                                PAGE64_USER) != PAGING64_OK) {
            vmspace64_destroy(&fresh);
            return (uint64_t)-12;
        }
    }

    /* Past this line the old process is being replaced, and there is
     * nothing left to return an error to. */
    /* A caught signal goes back to its default here, and an ignored one
     * stays ignored. The new image has never seen the old program's
     * handler addresses, so entering one lands wherever the new image
     * happens to put that address - and a process that installed no
     * handler of its own must get the default action rather than
     * somebody else's. */
    signal64_exec();

    old_space = p->space;
    p->space = fresh;
    proc64_set_current(p->pid);

    /* A parent suspended in vfork is waiting for exactly this moment -
     * not for the child to exit. The child has stopped standing on
     * anything the parent owns, so the parent may run again, and a
     * posix_spawn that waited for its child to *finish* would be a
     * fork/exec/wait rather than a spawn: wineboot starts the wineserver
     * and then talks to it. */
    vfork_release(p);
    p->brk_base = p->brk_current = info.brk_start;
    p->mmap_next = USPACE64_MMAP_BASE;
    kstrlcpy(p->exe_path, kpath, PROC64_PATH_MAX);

    /* The exe's own info either way - AT_PHDR and friends describe the
     * program, not the interpreter - plus where ld.so was put, which is
     * AT_BASE and how it finds itself. */
    rsp = uspace64_build_stack(&fresh, STACK_TOP, STACK_PAGES,
                               kargv, &info, interp_base, kenvp);

    for (uint64_t i = 0; i < sizeof(entry) / 8; i++)
        ((uint64_t*)&entry)[i] = 0;
    /* Into the interpreter when there is one: ld.so relocates the
     * program and calls its entry point itself. */
    entry.rip    = interp_base ? interp.entry : info.entry;
    entry.rsp    = rsp;
    entry.cs     = 0x23;
    entry.ss     = 0x1B;
    entry.rflags = 0x202;

    sched64_set_current_space(&fresh);
    execs++;
    vmspace64_switch(&fresh);
    /* The image that was replaced.
     *
     * execve builds the new space, points the process at it and jumps
     * in; nothing ever gave the old one back. That is a whole address
     * space per exec, and Wine execs constantly - the loader re-execs
     * itself to pick the loader for the target machine, the wineserver
     * is exec'd, and every process a prefix run starts is a fork
     * followed by one of these.
     *
     * After the switch, for the reason vmspace64_destroy refuses to do
     * it any other way: the old tables cannot be walked while they are
     * the tables being used to walk. And behind the same question the
     * exit path asks, because a vfork child stands in its parent's
     * space until exactly this moment - the parent's task still names
     * it, so sched64_space_in_use says so and the space survives. */
    if (!sched64_space_in_use(old_space.pml4_phys)) {
        uint64_t before = pmm64_free_frames();
        vmspace64_destroy(&old_space);
        reclaim_report("exec", before);
    }
    sched64_resume(&entry);                            /* never returns */
    return 0;
}

static uint64_t dispatch(syscall64_args_t* args) {
    uint64_t nr = args->nr;
    uint64_t a1 = args->a1, a2 = args->a2, a3 = args->a3;

    call_count++;

    /* A Win32 import, arriving through one of pe64.c's thunks. Checked
     * before the switch because the range is contiguous and has nothing
     * to do with Linux's numbering. */
    if (nr >= WIN32_64_BASE && nr <= WIN32_64_EXIT) {
        if (nr == WIN32_64_EXIT) {
            exit_code = a1;
            return a1;
        }
        return win32_64_call(nr, a1, a2, a3, args->a4);
    }

    switch (nr) {
    case SYS64_WRITE:
        return (uint64_t)fd_write_bytes(a1, (const char*)a2, a3);

    /* writev(fd, iov, iovcnt).
     *
     * This used to serve stdout and stderr and answer -EBADF for
     * anything else, on the reasoning that glibc's stdio was the only
     * thing that ever used it. The wineserver is the thing that proved
     * otherwise, and it did so in the most expensive way available: its
     * send_reply writes a reply with `write` when the reply carries no
     * extra data and with `writev` when it does, and a failed writev
     * takes the branch that kills the client's thread. So every reply
     * that fitted in the header worked, the prefix was built out of
     * them, and the first reply with a payload attached silently ended
     * the conversation - leaving wineboot blocked in recvmsg waiting
     * for an answer from a thread the server had already killed. */
    case SYS64_WRITEV: {
        const iovec64_t* iov = (const iovec64_t*)a2;
        int64_t total = 0;

        if (!iov && a3) return (uint64_t)-14;          /* -EFAULT */
        for (uint64_t i = 0; i < a3; i++) {
            int64_t w;
            if (!iov[i].iov_len) continue;
            w = fd_write_bytes(a1, (const char*)iov[i].iov_base,
                               iov[i].iov_len);
            /* An error on the first vector is the call's error; after
             * that, what has already gone is the answer. */
            if (w < 0) return total ? (uint64_t)total : (uint64_t)w;
            total += w;
            if ((uint64_t)w < iov[i].iov_len) break;   /* a short write */
        }
        return (uint64_t)total;
    }

    /* readv(fd, iov, iovcnt), for the same reason and by the same
     * route. */
    case SYS64_READV: {
        const iovec64_t* iov = (const iovec64_t*)a2;
        int64_t total = 0;

        if (!iov && a3) return (uint64_t)-14;
        for (uint64_t i = 0; i < a3; i++) {
            int64_t r;
            if (!iov[i].iov_len) continue;
            r = fd_read_bytes(args, nr, a1, iov[i].iov_base, iov[i].iov_len);
            if (r < 0) return total ? (uint64_t)total : (uint64_t)r;
            total += r;
            if (r == 0) break;                          /* end of file */
            if ((uint64_t)r < iov[i].iov_len) break;
        }
        return (uint64_t)total;
    }

    case SYS64_BRK:
        return uspace64_brk(a1);

    /* mmap(addr, length, prot, flags, fd, offset) */
    case SYS64_MMAP: {
        uint64_t flags = args->a4;
        uint64_t off   = args->a6;
        int64_t  fd    = (int64_t)args->a5;
        uint64_t mapped;
        int node;

        if (flags & MAP_ANONYMOUS)
            return uspace64_mmap(a1, a2, a3, flags);

        if (fd < 3 || fd >= FD_MAX || !fds[fd].used) return (uint64_t)-9;
        if (off & (PAGE64_SIZE - 1)) return (uint64_t)-22;  /* -EINVAL */
        node = fds[fd].node;

        /* /dev/fb0, decided before the rule below rather than after.
         *
         * This is the one mapping in this kernel that is genuinely
         * shared rather than copied: the whole point of mapping a
         * framebuffer is that the process's stores land on the screen,
         * so MAP_SHARED here is real and MAP_PRIVATE would be useless.
         * Ordering matters and is the bug this comment replaces - with
         * the file rule first, every writable MAP_SHARED was refused
         * before anyone asked what was being mapped, so /dev/fb0 could
         * be opened and described but never mapped.
         *
         * This is what a display driver above the kernel needs: Wine's
         * would open /dev/fb0, mmap it, and composite into it. */
        if (ramfs64_device(node) == RAMFS64_DEV_FB) {
            uint64_t span = fb64_bytes();
            if (!fb64_ready()) return (uint64_t)-19;        /* -ENODEV */
            if (off >= span) return (uint64_t)-22;
            if (a2 > span - off) return (uint64_t)-22;
            return uspace64_map_phys(a2, fb64_phys() + off, a3);
        }

        /* A window's pixels. The driver maps these once, at
         * WMIO_CREATE, and draws into them until the window is closed.
         *
         * Not uspace64_map_phys, because a window is a list of frames
         * rather than a run of physical memory - the allocator hands
         * them out one at a time and they are not adjacent.
         * uspace64_map_frames is the same call a shared file mapping
         * uses, and it takes the reference that keeps the frames alive
         * while the process has them mapped. */
        if (ramfs64_device(node) == RAMFS64_DEV_WM) {
            const uint64_t* frames;
            uint64_t n = 0, bytes = 0;
            int win = (int)fds[fd].pos - 1;

            frames = wmdev64_frames(win, &n, &bytes);
            if (!frames) return (uint64_t)-9;              /* -EBADF */
            if (off) return (uint64_t)-22;
            if (!a2 || a2 > bytes) return (uint64_t)-22;
            n = (a2 + PAGE64_SIZE - 1) / PAGE64_SIZE;
            return uspace64_map_frames(a1, (flags & MAP_FIXED) != 0,
                                       frames, n, a3);
        }

        /* --- an ordinary file mapping ---
         *
         * MAP_PRIVATE is a copy: the pages are the process's own, and
         * writing them does not change the file. That is exactly what a
         * loader wants, and it is what makes this implementable by
         * copying rather than by sharing frames - the filesystem keeps
         * a file's bytes in a heap allocation, which is not page
         * aligned and cannot be mapped directly.
         *
         * MAP_SHARED is genuinely shared (Milestone 78).
         *
         * It used to be refused whenever it was writable, on the
         * reasoning above: a file lives in a heap allocation, which is
         * not page aligned and cannot be mapped, so there was nothing to
         * share and nowhere to write back to. That is a statement about
         * how this filesystem stores a file, not about what MAP_SHARED
         * means, and the wineserver needs what it means - it hands every
         * client a descriptor for its session data and all of them map
         * it read-write and expect to see each other's stores.
         *
         * So the file is given physical frames and every process maps
         * those same frames. There is no writing back because there is
         * nowhere else for the bytes to live: the frames are the file.
         * A copy here would satisfy the call and lose the entire point,
         * which is the failure this kernel keeps producing and is worth
         * refusing to produce again. */
        if (flags & MAP_SHARED) {
            const uint64_t* frames;
            uint64_t nframes = 0, want;

            /* An offset would mean mapping from the middle of the frame
             * list, which is a small change and has no caller: refused
             * rather than half-answered. */
            if (off) return (uint64_t)-22;             /* -EINVAL */

            frames = ramfs64_frames(node, a2, &nframes);
            if (!frames) return (uint64_t)-12;         /* -ENOMEM */

            want = (a2 + PAGE64_SIZE - 1) / PAGE64_SIZE;
            if (nframes > want) nframes = want;
            shared_maps++;
            /* The frame behind KUSER_SHARED_DATA, named by what actually
             * backs it rather than by a constant: which frame ramfs
             * hands out depends on everything allocated before it, so
             * hard-coding the address measured in one run would watch
             * the wrong page in the next. */
            if (a1 == 0x7ffe0000ULL && nframes) {
                pmm64_watch_frame(frames[0]);
                syscall64_watch_usd(frames[0]);
            }
            /* Which file, and which physical frame it starts at.
             *
             * A shared mapping is only shared if two mappings of the
             * same file land on the same frames, and that is exactly
             * what cannot be read off an address. Wine maps its
             * KUSER_SHARED_DATA read-only at 0x7ffe0000 and then maps
             * the same section again read-write somewhere else, purely
             * to store one byte - SystemCall = 1 - which every PE
             * syscall stub then tests to decide whether to call Wine's
             * dispatcher or execute a real `syscall` instruction. The
             * guest is taking the second branch, so that byte is
             * reading 0, and whether the two mappings share a frame is
             * the question that decides why. */
            if (trace) {
                uint64_t res_dbg = uspace64_map_frames(a1,
                                       (flags & MAP_FIXED) != 0,
                                       frames, nframes, a3);
                serial64_puts("NOVARIS64: [shmap node ");
                serial64_putdec((uint64_t)node);
                serial64_puts(" frame0 ");
                serial64_puthex(nframes ? frames[0] : 0);
                serial64_puts(" -> ");
                serial64_puthex(res_dbg);
                /* The byte the Wine syscall stubs test, read through
                 * the mapping that was just made. Following it across
                 * processes says who writes it and who sees it, which
                 * is the whole question: it must be 1 by the time any
                 * PE stub runs, and it is reading 0. */
                if ((int64_t)res_dbg > 0 && nframes &&
                    user_range_ok(res_dbg + 0x308, 1)) {
                    serial64_puts(" byte308=");
                    serial64_putdec(*(volatile uint8_t*)(res_dbg + 0x308));
                }
                /* The first eight bytes of whatever was just mapped.
                 *
                 * win32u maps Wine's session shared memory and then
                 * reports that every object it looks up there has the
                 * wrong id - many different expected ids, none of them
                 * found, which is what a page of zeros looks like from
                 * the far side. The frame is shared: every process maps
                 * the same one, and three of them map it writable. So
                 * the question is whether anything is ever written into
                 * it, and the frame can simply be read to find out. */
                if ((int64_t)res_dbg > 0 && nframes &&
                    user_range_ok(res_dbg, 8)) {
                    serial64_puts(" head=");
                    serial64_puthex(*(volatile uint64_t*)res_dbg);
                }
                serial64_putc('\n');
                return res_dbg;
            }
            return uspace64_map_frames(a1, (flags & MAP_FIXED) != 0,
                                       frames, nframes, a3);
        }

        /* MAP_PRIVATE of a file, copy-on-write, which is what it means.
         *
         * The bytes used to be read out of the file here, once, and the
         * mapping was a snapshot from then on. That is not what Linux
         * does and the difference is not academic: a reader that never
         * writes goes on seeing the file's current contents, and Wine
         * depends on it. wineserver keeps its session shared memory in
         * a file, maps it MAP_SHARED read-write and writes objects into
         * it; every client maps that same file MAP_PRIVATE and only
         * reads. Measured, one prefix run: seventy-one clients, each
         * given a copy taken before the objects existed. What it looked
         * like was `Session object id doesn't match expected id`, a
         * different id every time, 1,026 window classes that could not
         * be registered, no desktop window, and a mapping that reported
         * success throughout.
         *
         * Only from the start of the file, because ramfs64_frames
         * counts from there and an offset would mean mapping from the
         * middle of the list. Every caller that needs this maps from
         * zero; a PE section mapped at an offset still takes the copy
         * below, which is wasteful and correct, because a module file
         * does not change under its reader. */
        /* A writable private mapping belongs here too, and is in fact
         * the case that matters: Wine maps the session PROT_READ |
         * PROT_WRITE and then only reads it. Copy-on-write is exactly
         * that bargain - the permission is real, and the copy is taken
         * when it is used rather than before. PAGE64_COW_RW carries the
         * permission so break_cow can tell that write from a fault. */
        if (off == 0) {
            uint64_t nframes = 0;
            const uint64_t* frames = ramfs64_frames(node, a2, &nframes);
            uint64_t want = (a2 + PAGE64_SIZE - 1) / PAGE64_SIZE;

            /* All of it, or none of it. A mapping that runs past the end
             * of the file needs zero pages for the tail, and mixing the
             * two here would leave a half-built mapping to unpick on
             * failure. */
            if (frames && nframes >= want) {
                uint64_t r = uspace64_map_frames_cow(a1,
                                 (flags & MAP_FIXED) != 0,
                                 frames, want, a3);
                if ((int64_t)r > 0) {
                    file_maps++;
                    return r;
                }
            }
        }

        /* Mapped writable whatever the caller asked for, because the
         * kernel is about to write the file's contents into it. */
        mapped = uspace64_mmap(a1, a2, PROT_READ | PROT_WRITE,
                               flags | MAP_ANONYMOUS);
        if ((int64_t)mapped < 0) return mapped;

        /* The pages arrive zeroed, so a mapping that runs past the end
         * of the file reads as zeros there - which is what Linux does
         * for the tail of the last page. Beyond that last page Linux
         * raises SIGBUS; this does not, and simply keeps reading zeros. */
        {
            uint64_t avail = ramfs64_size(node);
            uint64_t want  = a2;
            if (off < avail) {
                if (off + want > avail) want = avail - off;
                ramfs64_read(node, off, (void*)mapped, want);
            }
        }

        if (!(a3 & PROT_WRITE))
            uspace64_protect(mapped, a2, a3);

        /* Which file was copied, and how much of it.
         *
         * This branch is a snapshot: the bytes are read out of the file
         * once, here, and a later write by anyone else is invisible to
         * the mapping. Linux's MAP_PRIVATE is copy-on-write, so a reader
         * that never writes goes on seeing the file's current contents -
         * and a program that maps a file another process keeps updating,
         * read-only, is relying on exactly that.
         *
         * win32u maps Wine's session shared memory read-only and then
         * cannot find a single object in it, with the ids it expects
         * changing every time. A frozen copy would look precisely like
         * that. The trace says whether the session is one of the files
         * arriving here, which is the difference between that being the
         * explanation and being a plausible story. */
        if (trace) {
            serial64_puts("NOVARIS64: [privmap node ");
            serial64_putdec((uint64_t)node);
            serial64_puts(" len ");
            serial64_puthex(a2);
            serial64_puts(" size ");
            serial64_puthex(ramfs64_size(node));
            serial64_puts(" prot ");
            serial64_puthex(a3);
            serial64_puts(" -> ");
            serial64_puthex(mapped);
            serial64_putc('\n');
        }

        file_maps++;
        return mapped;
    }

    case SYS64_MUNMAP:
        return uspace64_munmap(a1, a2);

    case SYS64_MPROTECT:
        /* This used to be accepted and ignored, on the reasoning that
         * every mapping is already readable and writable so the only
         * thing left to implement was taking permissions away. That was
         * wrong in both directions, and Wine found the other one:
         * mappings made read-only - a loader's file mapping, or a page
         * left shared by fork - are made writable again through here,
         * and answering "done" without doing it turns the next store
         * into an unexplained fault a long way from this call.
         *
         * uspace64_protect says what it does and does not honour. */
        if (a1 & (PAGE64_SIZE - 1)) return (uint64_t)-22;   /* -EINVAL */
        uspace64_protect(a1, a2, a3);
        return 0;

    /* arch_prctl(ARCH_SET_FS, addr) is how a thread pointer is set on
     * x86-64, and glibc does it before it can touch a single piece of
     * thread-local storage - errno included. Nothing works before this. */
    case SYS64_ARCH_PRCTL:
        switch (a1) {
        case 0x1002:                                   /* ARCH_SET_FS */
            write_msr(0xC0000100u, a2);
            return 0;
        case 0x1001:                                   /* ARCH_SET_GS */
            write_msr(0xC0000101u, a2);
            return 0;

        /* And the getters, which were missing.
         *
         * Wine reads the GS base from inside its own signal handler -
         * GS is where the Windows TEB lives on x86-64, and a handler
         * that has been entered on an alternate stack has to recover it
         * from somewhere. arch_prctl(ARCH_GET_GS) answering -EINVAL
         * means it recovers nothing, and what follows is a rt_sigreturn
         * that restores a register set built around the answer it did
         * not get:
         *
         *     158(0x1004, ...) = -22        ARCH_GET_GS
         *     15(...)                       rt_sigreturn
         *     35(0xffffffffffffffda, ...)   nanosleep(-38, ...)
         *
         * -38 being -ENOSYS, used as a struct timespec *, one syscall
         * after the context was restored. */
        case 0x1003:                                   /* ARCH_GET_FS */
            if (!a2) return (uint64_t)-14;             /* -EFAULT */
            *(uint64_t*)a2 = read_msr(0xC0000100u);
            return 0;
        case 0x1004:                                   /* ARCH_GET_GS */
            if (!a2) return (uint64_t)-14;             /* -EFAULT */
            *(uint64_t*)a2 = read_msr(0xC0000101u);
            return 0;
        default:
            return (uint64_t)-22;                      /* -EINVAL */
        }

    /* clone(flags, child_stack, parent_tid, child_tid, tls)
     *
     * Threads only - the CLONE_VM|CLONE_THREAD case. A fork, which is
     * the same syscall without CLONE_VM, would have to copy the address
     * space; nothing here does that, so it is refused rather than
     * silently producing a second thread where a child process was
     * asked for.
     *
     * The child is its parent with three differences, which is exactly
     * what Linux specifies: a different stack, a thread pointer of its
     * own, and rax = 0 so the two can tell each other apart on return.
     * Everything else is inherited, which is why the entry stub saves
     * the callee-saved registers it is otherwise entitled to ignore. */
    case SYS64_CLONE: {
        registers64_t child;
        const vmspace64_t* space;
        int tid;

        /* clone without CLONE_VM is a fork, and this is not a corner
         * case: it is how glibc implements fork(2). There is no
         * fork(2) in glibc's source - it calls
         * clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD) - so a
         * kernel that answers -ENOSYS here has no fork as far as any
         * ordinary C program is concerned, however complete its
         * SYS_fork happens to be. That is exactly what this kernel
         * was: the raw-assembly fork test passed and every glibc
         * program's fork() failed. */
        if (!(a1 & CLONE_VM)) return do_fork(args);

        /* CLONE_VM with CLONE_VFORK is not a thread, it is a spawn.
         * glibc's posix_spawn issues exactly this, and it is how Wine
         * starts the wineserver - so answering it the way the thread
         * path below does produces a tid that wait4 cannot find, and a
         * parent that gives up with -ECHILD. That is precisely where
         * Milestone 72 left wineboot.
         *
         * The child needs a pid of its own and a parent that stays out
         * of its way, which is a fork, and the stack it was handed. */
        if (a1 & CLONE_VFORK) return do_fork_common(args, a2, 1);

        if (!a2)                 return (uint64_t)-22;  /* -EINVAL       */

        space = sched64_current_space();
        if (!space) return (uint64_t)-38;

        child.rax = 0;            /* how the child knows it is the child */
        child.rbx = args->rbx;
        child.rcx = 0;            /* SYSCALL destroyed it in the parent too */
        child.rdx = a3;
        child.rsi = a2;
        child.rdi = a1;
        child.rbp = args->rbp;
        child.r8  = args->a5;
        child.r9  = args->a6;
        child.r10 = args->a4;
        child.r11 = 0;
        child.r12 = args->r12;
        child.r13 = args->r13;
        child.r14 = args->r14;
        child.r15 = args->r15;

        child.int_no   = 0;
        child.err_code = 0;
        child.rip      = args->ret_rip;      /* just after its `syscall` */
        child.cs       = 0x23;
        child.rflags   = args->ret_rflags | 0x200;   /* IF, or it never
                                                      * yields the CPU  */
        child.rsp      = a2;
        child.ss       = 0x1B;

        tid = sched64_add_frame(&child, space,
                                (a1 & CLONE_SETTLS) ? args->a5 : 0);
        if (tid < 0) return (uint64_t)-11;              /* -EAGAIN */

        /* The parent gets the child's tid; the child gets 0 above. */
        return (uint64_t)tid + 1;
    }

    /* fork(2). The child is the parent with a different address space,
     * a different pid, and 0 where the parent gets the child's pid.
     *
     * Ordering matters: the address space copy is the expensive part
     * and the part that can fail, so it happens before anything is
     * committed. A half-built process is worse than a failed fork. */
    case SYS64_FORK:
        return do_fork(args);

    /* execve(path, argv, envp). Replaces the calling process rather
     * than returning to it, so on success there is nothing to return
     * to: the syscall ends by entering the new program. */
    case SYS64_EXECVE:
        return do_execve((const char*)a1, (const char* const*)a2,
                         (const char* const*)a3, args);

    case SYS64_WAIT4: {
        int status = 0;
        int reaped = proc64_reap_child(proc64_current_pid(), &status);
        registers64_t self, next;
        vmspace64_t next_space;
        uint64_t next_fs;

        if (reaped >= 0) {
            /* wait4 reports a *wait status*, not an exit code: the low
             * byte says how it died and the next says with what. A
             * caller using WEXITSTATUS shifts it back down. */
            if (a2) *(int*)a2 = (status & 0xFF) << 8;
            return (uint64_t)reaped;
        }

        /* Nothing to reap. If there are no children at all that is
         * -ECHILD; if there are, the caller has to *wait* - which is
         * the entire point of the call, and returning -ECHILD to a
         * parent whose child simply has not been scheduled yet is the
         * difference between a working fork and a racing one. */
        if (!proc64_has_children(proc64_current_pid()))
            return (uint64_t)-10;                      /* -ECHILD */

        /* Blocked, and restarted rather than resumed: rewind rip by the
         * two bytes of the `syscall` instruction and put the number
         * back in rax, so waking re-executes the call and re-checks.
         * Linux does the same thing for a restartable syscall, for the
         * same reason - there is no other way to return a value that
         * was not known when the caller blocked. */
        frame_from_args(args, SYS64_WAIT4, &self);
        self.rip = args->ret_rip - 2;

        if (!sched64_block_current(&self, PROC64_WAIT_KEY(proc64_current_pid()), SYS64_WAIT4,
                                   &next, &next_space, &next_fs))
            return (uint64_t)-10;

        vmspace64_switch(&next_space);
        write_msr(0xC0000100u, next_fs);
        sched64_resume(&next);                         /* never returns */
    }

    case SYS64_GETPID:
        return (uint64_t)proc64_current_pid();

    case SYS64_GETPPID: {
        proc64_t* p = proc64_current();
        return p ? (uint64_t)p->parent : 0;
    }

    /* Who the process is. There is one user here and it is root, which
     * is also the owner do_stat reports for every file - and the two
     * answers have to agree, because that comparison is how Wine decides
     * whether it may create its prefix:
     *
     *     if (!stat( config_dir, &st ) && st.st_uid != getuid())
     *         fatal_error( "'%s' is not owned by you, refusing to
     *                       create a configuration directory there" );
     *
     * These are the syscalls Linux specifies as never failing, so glibc
     * does not check them and hands back whatever the kernel returned.
     * Left unimplemented, getuid() returned -ENOSYS as a uid, which is
     * not 0, and wineboot refused to build a prefix in a directory it
     * owned. A refusal, reported clearly, from a missing four-line
     * syscall. */
    case SYS64_GETUID:
    case SYS64_GETEUID:
    case SYS64_GETGID:
    case SYS64_GETEGID:
        return 0;

    case SYS64_GETTID:
        return (uint64_t)sched64_current() + 1;

    case SYS64_SET_TID_ADDRESS:
        return (uint64_t)sched64_current() + 1;

    case SYS64_SET_ROBUST_LIST:
        return 0;

    case SYS64_IOCTL: {
        /* The two fbdev queries, with Linux's own structure layouts -
         * offsets taken from <linux/fb.h> for x86-64. A program asks the
         * driver what the screen is rather than being told out of band,
         * because that is the interface everything from SDL to Wine's
         * fbdev path already speaks.
         *
         * Getting these offsets wrong is the classic way to produce a
         * driver that "works" and paints diagonal stripes: xres and
         * line_length land in the wrong fields and every scanline is
         * placed from a wrong stride. The differential test in
         * userland/fbdraw64.c checks the values, not just the call. */
        /* /dev/wm, which is how a display driver above this kernel asks
         * for a surface. The window is the descriptor's, so it is kept
         * in the descriptor: each open of /dev/wm is one window, which
         * is winenovaris.drv's own model - its win_data holds one fd per
         * window - and it means two windows in one process cannot be
         * confused for each other. `pos` is the file offset for an
         * ordinary file and unused for a device, so it carries the
         * window index, biased by one so that zero still means "no
         * window yet" on a descriptor that has only just been opened. */
        if (a1 >= 3 && a1 < FD_MAX && fds[a1].used &&
            ramfs64_device(fds[a1].node) == RAMFS64_DEV_WM) {
            int win = (int)fds[a1].pos - 1;
            int r;

            if (!a3) return (uint64_t)-14;                /* -EFAULT */
            if (!user_range_ok(a3, 4)) return (uint64_t)-14;

            switch ((uint32_t)a2) {
            case WMIO64_SCREEN:
                return (uint64_t)(int64_t)wmdev64_screen(
                           (struct wm64_rect*)a3);
            case WMIO64_CREATE:
                if (win >= 0) return (uint64_t)-22;       /* already has one */
                r = wmdev64_create((const struct wm64_create*)a3);
                if (r < 0) return (uint64_t)(int64_t)r;
                fds[a1].pos = (uint64_t)(r + 1);
                return 0;
            case WMIO64_GETINFO:
                return (uint64_t)(int64_t)wmdev64_getinfo(
                           win, (struct wm64_info*)a3);
            case WMIO64_GETSIZE:
                return (uint64_t)(int64_t)wmdev64_getsize(
                           win, (struct wm64_rect*)a3);
            case WMIO64_TITLE:
                return (uint64_t)(int64_t)wmdev64_title(win, (const char*)a3);
            case WMIO64_DAMAGE:
                return (uint64_t)(int64_t)wmdev64_damage(
                           win, (const struct wm64_rect*)a3);
            case WMIO64_POLL:
                return (uint64_t)(int64_t)wmdev64_poll(
                           win, (struct wm64_event*)a3);
            default:
                return (uint64_t)-25;                     /* -ENOTTY */
            }
        }

        #define FBIOGET_VSCREENINFO 0x4600
        #define FBIOGET_FSCREENINFO 0x4602

        if ((a2 == FBIOGET_VSCREENINFO || a2 == FBIOGET_FSCREENINFO) &&
            a1 >= 3 && a1 < FD_MAX && fds[a1].used &&
            ramfs64_device(fds[a1].node) == RAMFS64_DEV_FB) {
            uint8_t* out = (uint8_t*)a3;
            uint32_t i;

            if (!fb64_ready()) return (uint64_t)-19;       /* -ENODEV */
            if (!out) return (uint64_t)-14;               /* -EFAULT */

            if (a2 == FBIOGET_VSCREENINFO) {
                for (i = 0; i < 160; i++) out[i] = 0;
                *(uint32_t*)(out +  0) = fb64_width();     /* xres */
                *(uint32_t*)(out +  4) = fb64_height();    /* yres */
                *(uint32_t*)(out +  8) = fb64_width();     /* xres_virtual */
                *(uint32_t*)(out + 12) = fb64_height();    /* yres_virtual */
                *(uint32_t*)(out + 24) = fb64_bpp();       /* bits_per_pixel */
                /* The colour layout, as bit offsets into a pixel. This
                 * is what says 0x00RRGGBB rather than leaving a caller
                 * to guess from the depth. */
                *(uint32_t*)(out + 32) = 16; *(uint32_t*)(out + 36) = 8; /* red */
                *(uint32_t*)(out + 44) =  8; *(uint32_t*)(out + 48) = 8; /* green */
                *(uint32_t*)(out + 56) =  0; *(uint32_t*)(out + 60) = 8; /* blue */
                /* transp length stays 0: the fourth byte is padding this
                 * driver writes as zero, not an alpha channel. */
                return 0;
            }

            for (i = 0; i < 80; i++) out[i] = 0;
            {
                static const char id[] = "novarisfb";
                for (i = 0; id[i]; i++) out[i] = (uint8_t)id[i];
            }
            *(uint64_t*)(out + 16) = fb64_phys();          /* smem_start */
            *(uint32_t*)(out + 24) = (uint32_t)fb64_bytes(); /* smem_len */
            *(uint32_t*)(out + 28) = 0;                    /* PACKED_PIXELS */
            *(uint32_t*)(out + 36) = 2;                    /* TRUECOLOR */
            *(uint32_t*)(out + 48) = fb64_pitch();         /* line_length */
            return 0;
        }

        /* glibc asks whether fd 1 is a terminal to choose line
         * buffering. -ENOTTY makes it a fully buffered stream, which is
         * correct here: this is a serial port, not a tty. */
        return (uint64_t)-25;
    }

    case SYS64_FSTAT: {
        uint8_t* st = (uint8_t*)a2;
        for (int i = 0; i < 144; i++) st[i] = 0;

        /* A real file has to report itself as one. Until Milestone 61
         * this answered "character device, size 0" for every descriptor,
         * which is fine for stdout and fatal for a library: ld.so fstats
         * the file it has just opened, sees something unmappable with no
         * length, and gives up without ever mapping libc - which
         * presents much later as an undefined symbol. */
        if (a1 >= 3) {
            if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
            fill_ids(st, fds[a1].node);
            *(uint32_t*)(st + 24) = ramfs64_is_dir(fds[a1].node)
                                    ? 0040755u : 0100644u;
            *(uint64_t*)(st + 48) = ramfs64_size(fds[a1].node);
            *(uint64_t*)(st + 56) = 4096;
            return 0;
        }

        /* stdin/stdout/stderr: a character device, which is what makes
         * glibc's stdio choose the buffering it does. */
        *(uint32_t*)(st + 24) = 0020620u;  /* S_IFCHR | 0620 */
        *(uint64_t*)(st + 56) = 1024;
        return 0;
    }

    case SYS64_GETRANDOM: {
        /* Not random. It is deterministic and it is documented as such,
         * because a kernel with no entropy source that pretends
         * otherwise is worse than one that says so. glibc uses this for
         * the stack guard, which this makes predictable. */
        uint8_t* buf = (uint8_t*)a1;
        static uint64_t seed = 0x2545F4914F6CDD1DULL;
        for (uint64_t i = 0; i < a2; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            buf[i] = (uint8_t)seed;
        }
        return a2;
    }

    /* futex(uaddr, op, val, timeout, uaddr2, val3)
     *
     * Two operations, which is what a thread library actually needs:
     * WAIT blocks until somebody WAKEs the same address.
     *
     * FUTEX_PRIVATE_FLAG is masked off rather than acted on. It tells
     * Linux the futex is not shared between processes, which lets it
     * skip looking the page up in a global hash - an optimisation, not a
     * semantic. With one address space per process and no shared memory
     * here, private and shared behave identically. */
    case SYS64_FUTEX: {
        uint32_t op = (uint32_t)a2 & ~(uint32_t)FUTEX_PRIVATE_FLAG;

        if (op == FUTEX_WAIT) {
            registers64_t self, next;
            vmspace64_t next_space;
            uint64_t next_fs;

            /* Whether this wait was given a deadline, and whether it was
             * shared between processes.
             *
             * This kernel has no timeout for FUTEX_WAIT at all -
             * sched64_block_current takes an address and a wake value
             * and nothing else - so a wait that Linux would expire
             * blocks here forever. And the key is the *virtual*
             * address, which is right for a private futex and wrong for
             * one in memory two processes map at different addresses.
             *
             * Both would deadlock exactly as observed: services.exe and
             * winedevice.exe parked in futex while the system idles.
             * Counting says which, and whether either is even reached,
             * before either is built. */
            if (args->a4) futex_timed++;
            if (!((uint32_t)a2 & (uint32_t)FUTEX_PRIVATE_FLAG)) futex_shared++;

            /* A timed wait that has already begun.
             *
             * Measured across a prefix run: 223 of 251 waits carry a
             * timeout, and this kernel had none - sched64_block_current
             * takes an address and a wake value and has no deadline at
             * all - so every one of them was a wait that Linux would
             * expire and this kernel would not. That is what stopped
             * services.exe: it and winedevice.exe sat in futex while
             * the wineserver idled and wineboot waited, INFINITE, for
             * the started event neither would ever set.
             *
             * The value is re-checked on each turn because that is the
             * contract the interface is built on - a waker changes the
             * word and then wakes, which is why FUTEX_WAIT is given the
             * value it expects to find. Restarting rather than blocking
             * means a deadline can be noticed at all; the cost is that
             * the thread yields around the loop instead of sleeping,
             * which is what nanosleep and poll already do here.
             *
             * Every futex in the run is PRIVATE - zero without the flag
             * - so the virtual address is the right key and is left
             * alone. It would be the wrong key for a futex two
             * processes map at different addresses, and nothing here
             * has one. */
            if (wait_started() && wait_call[wait_slot()] == nr) {
                if (*(volatile uint32_t*)a1 != (uint32_t)a3) {
                    wait_done();
                    return 0;                      /* woken */
                }
                if (wait_expired()) {
                    wait_done();
                    return (uint64_t)-110;         /* -ETIMEDOUT */
                }
                return wait_restart(args, nr, wait_deadline[wait_slot()]);
            }

            /* The comparison is the whole point of the interface, and
             * it is why futex has no race: between the caller deciding
             * to sleep and this check, the waker may already have run
             * and changed the value. If it has, do not sleep. */
            if (*(volatile uint32_t*)a1 != (uint32_t)a3)
                return (uint64_t)-11;              /* -EAGAIN */

            if (args->a4) {
                const struct { uint64_t sec, nsec; }* ts =
                    (const void*)args->a4;
                uint64_t want;

                if (!user_range_ok(args->a4, sizeof(*ts)))
                    return (uint64_t)-14;          /* -EFAULT */
                want = ts->sec * CLOCK64_HZ
                     + ts->nsec / (1000000000ull / CLOCK64_HZ);
                /* A deadline already past still gets one turn, so that
                 * a zero timeout polls once rather than reporting a
                 * timeout for a word it never looked at. */
                futex_waits++;
                return wait_restart(args, nr,
                                    clock64_ticks() + (want ? want : 1));
            }

            futex_waits++;
            frame_from_args(args, 0, &self);
            if (!sched64_block_current(&self, a1, 0, &next, &next_space,
                                       &next_fs))
                return (uint64_t)-35;              /* -EDEADLK */

            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);
            sched64_resume(&next);                 /* never returns */
        }

        if (op == FUTEX_WAKE) {
            int n = sched64_wake(a1, (int)(uint32_t)a3);
            futex_wakes += (uint64_t)n;
            return (uint64_t)n;
        }

        /* FUTEX_REQUEUE, FUTEX_WAIT_BITSET, priority inheritance and
         * the rest are not implemented; -ENOSYS is what lets a library
         * fall back rather than assume they worked. */
        return (uint64_t)-38;
    }

    case SYS64_RT_SIGACTION:
        /* Real since Milestone 58: a handler installed here is a handler
         * a fault will actually reach. */
        return (uint64_t)(int64_t)signal64_sigaction(
                   (int)a1, (const ksigaction64_t*)a2,
                   (ksigaction64_t*)a3);

    /* sigaltstack(ss, oss).
     *
     * Wine asks for one in init_thread_pipe, at every thread start, and
     * does not check the answer - so -ENOSYS here was invisible until a
     * thread overflowed its stack and the kernel tried to write the
     * signal frame below the stack pointer that had just faulted. See
     * signal64_deliver. */
    case SYS64_SIGALTSTACK:
        return (uint64_t)(int64_t)signal64_sigaltstack(
                   (const altstack64_t*)a1, (altstack64_t*)a2,
                   saved_user_rsp);

    case SYS64_RT_SIGRETURN: {
        registers64_t resumed;
        /* The frame sits on the thread's own stack, which is where the
         * handler is returning from. Resuming it is the same trick as
         * futex: this syscall does not return, the thread does. */
        signal64_sigreturn(saved_user_rsp, &resumed);
        /* The scheduler's saved copy for this task is stale now, and
         * harmlessly so: the next timer tick overwrites it from the
         * live frame before anything switches away. */
        sched64_resume(&resumed);                 /* never returns */
    }

    case SYS64_RT_SIGPROCMASK:
        /* Still accepted and ignored. Masking matters for asynchronous
         * signals; a fault is delivered to the thread that caused it
         * whether or not it wanted to hear about it. */
        return 0;

    /* --- files ---------------------------------------------------- */

    case SYS64_OPEN: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return do_open(path, a2, (uint32_t)a3);
    }

    case SYS64_OPENAT: {
        /* openat(dirfd, path, flags, mode). glibc uses this in
         * preference to open(2).
         *
         * AT_FDCWD means "relative to the working directory", which is
         * now a thing this kernel has. A real dirfd - relative to some
         * other open directory - is answered for the case that actually
         * occurs, an absolute path, where the dirfd is ignored by
         * definition; anything else is -EBADF rather than silently
         * resolved against the wrong directory. */
        char path[PROC64_PATH_MAX];
        const char* given = (const char*)a2;
        int64_t e;

        if ((int64_t)(int32_t)a1 != AT_FDCWD_ && given && given[0] != '/')
            return (uint64_t)-9;                       /* -EBADF */

        e = abs_path(given, path);
        if (e) return (uint64_t)e;
        return do_open(path, a3, (uint32_t)args->a4);
    }

    /* chdir(path). Wine's first act on a prefix is to chdir into it, and
     * the 32-bit tree's log is full of "chdir to /disk/.wine : No such
     * file or directory" from before it had one. */
    case SYS64_CHDIR: {
        char path[PROC64_PATH_MAX];
        proc64_t* me = proc64_current();
        int node;
        int64_t e;

        if (!me) return (uint64_t)-2;
        e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;

        node = ramfs64_lookup(path);
        if (node < 0) return (uint64_t)-2;             /* -ENOENT */
        if (!ramfs64_is_dir(node)) return (uint64_t)-20; /* -ENOTDIR */

        /* Stored as the canonical path built back up from the node, not
         * as the text the program supplied - so that getcwd after
         * chdir("a/../b") answers "/b", and after a chdir through a
         * symlink answers where it landed rather than how it got
         * there. */
        if (ramfs64_path(node, me->cwd, PROC64_PATH_MAX) < 0)
            return (uint64_t)-36;                      /* -ENAMETOOLONG */
        return 0;
    }

    case SYS64_FCHDIR: {
        proc64_t* me = proc64_current();
        if (!me) return (uint64_t)-2;
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (!ramfs64_is_dir(fds[a1].node)) return (uint64_t)-20;
        if (ramfs64_path(fds[a1].node, me->cwd, PROC64_PATH_MAX) < 0)
            return (uint64_t)-36;
        return 0;
    }

    /* getcwd(buf, size). Returns the length including the NUL, which is
     * the kernel's contract and not glibc's - glibc returns the pointer
     * and gets the length from here. */
    case SYS64_GETCWD: {
        proc64_t* me = proc64_current();
        char* buf = (char*)a1;
        uint64_t n;

        if (!me || !buf) return (uint64_t)-14;         /* -EFAULT */
        n = kstrlen(me->cwd) + 1;
        if (a2 < n) return (uint64_t)-34;              /* -ERANGE */
        kmemcpy(buf, me->cwd, n);
        return n;
    }

    case SYS64_SYMLINK: {
        /* symlink(target, linkpath) - the target is not a path this
         * kernel resolves, so only the second argument is made
         * absolute. */
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        e = ramfs64_symlink(path, (const char*)a1);
        return e < 0 ? (uint64_t)e : 0;
    }

    case SYS64_SYMLINKAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a2 != AT_FDCWD_
            && ((const char*)a3)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a3, path);
        if (e) return (uint64_t)e;
        e = ramfs64_symlink(path, (const char*)a1);
        return e < 0 ? (uint64_t)e : 0;
    }

    case SYS64_CLOSE:
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        fd_release(a1);
        return 0;

    /* pipe2(int fds[2], flags). The wineserver's first act, and where
     * Milestone 73 stopped. glibc's pipe(2) is this with flags 0. */
    case SYS64_PIPE2: {
        int* out = (int*)a1;
        int p, rd, wr;

        if (!out) return (uint64_t)-14;                /* -EFAULT */

        p = pipe64_create();
        if (p < 0) return (uint64_t)-23;               /* -ENFILE */

        rd = fd_alloc(3);
        if (rd < 0) { pipe64_unref(p, 0, 0); return (uint64_t)-24; }
        /* Claimed before the second allocation, or both ends come back
         * as the same descriptor. */
        fd_init_pipe(rd, p, -1, (a2 & O_NONBLOCK) != 0, (a2 & O_CLOEXEC) != 0);

        wr = fd_alloc(3);
        if (wr < 0) { fd_release(rd); return (uint64_t)-24; }
        fd_init_pipe(wr, -1, p, (a2 & O_NONBLOCK) != 0, (a2 & O_CLOEXEC) != 0);

        out[0] = rd;
        out[1] = wr;
        pipes_made++;
        return 0;
    }

    /* --- the listening socket (Milestone 75) ---------------------- *
     *
     * A socketpair is two ends handed to somebody who already holds
     * both. These five calls are the other arrangement: a name in the
     * filesystem that two processes which have never met can find, a
     * queue of callers waiting at it, and an accept that makes a fresh
     * crossed pair per caller. Once the pair exists it is the descriptor
     * layer from Milestone 74 and nothing here is involved again. */
    case SYS64_SOCKET: {
        int s, fd;

        if (a1 != AF_UNIX_) return (uint64_t)-97;      /* -EAFNOSUPPORT */
        if ((int)(a2 & 0xFF) != SOCK_STREAM_) return (uint64_t)-94;

        s = sock64_create();
        if (s < 0) return (uint64_t)-23;               /* -ENFILE */
        fd = fd_alloc(3);
        if (fd < 0) { sock64_destroy(s); return (uint64_t)-24; }

        fd_adopt_pipe(fd, -1, -1, s,
                      (a2 & O_NONBLOCK) != 0, (a2 & O_CLOEXEC) != 0);
        sockets_made++;
        return (uint64_t)fd;
    }

    /* bind(fd, addr, addrlen). The name becomes a node in the
     * filesystem, and it is the node rather than the text that
     * identifies the socket - so a client that reaches it by a different
     * but equivalent path finds the same listener. */
    case SYS64_BIND: {
        const sockaddr_un64_t* addr = (const sockaddr_un64_t*)a2;
        char path[PROC64_PATH_MAX];
        int64_t e;
        int node;

        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (fds[a1].kind != FD64_SOCKET || fds[a1].sock < 0)
            return (uint64_t)-88;                      /* -ENOTSOCK */
        if (!addr || addr->sun_family != AF_UNIX_) return (uint64_t)-22;

        e = abs_path(addr->sun_path, path);
        if (e) return (uint64_t)e;

        /* Already there. Wine unlinks a stale socket before binding and
         * treats -EADDRINUSE as "somebody else got the lock", so this
         * has to be the error rather than a silent replacement. */
        if (ramfs64_lookup_nofollow(path) >= 0) return (uint64_t)-98;

        node = ramfs64_create(path, 0);
        if (node < 0) return (uint64_t)-28;            /* -ENOSPC */
        ramfs64_set_device(node, RAMFS64_DEV_SOCK);
        ramfs64_set_mode(node, 0755);

        return (uint64_t)sock64_bind(fds[a1].sock, node);
    }

    case SYS64_LISTEN:
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (fds[a1].kind != FD64_SOCKET || fds[a1].sock < 0)
            return (uint64_t)-88;
        return (uint64_t)sock64_listen(fds[a1].sock, (int)a2);

    /* connect(fd, addr, addrlen).
     *
     * Completes immediately or not at all: the pair is made here and the
     * far half left on the listener's queue, so there is no state in
     * which a connection is half open. Linux can return -EINPROGRESS on
     * a non-blocking socket; nothing here ever does, which is a
     * simplification rather than a lie - the connection really is
     * established when this returns 0. */
    case SYS64_CONNECT: {
        const sockaddr_un64_t* addr = (const sockaddr_un64_t*)a2;
        char path[PROC64_PATH_MAX];
        int64_t e;
        int node, listener, rx = -1, tx = -1, rc;

        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (fds[a1].kind != FD64_SOCKET || fds[a1].sock < 0)
            return (uint64_t)-88;
        if (!addr || addr->sun_family != AF_UNIX_) return (uint64_t)-22;

        e = abs_path(addr->sun_path, path);
        if (e) return (uint64_t)e;

        node = ramfs64_lookup_nofollow(path);
        if (node < 0) return (uint64_t)-2;             /* -ENOENT */
        if (ramfs64_device(node) != RAMFS64_DEV_SOCK)
            return (uint64_t)-111;                     /* -ECONNREFUSED */

        listener = sock64_listener_for_node(node);
        if (listener < 0) return (uint64_t)-111;

        rc = sock64_connect(fds[a1].sock, listener, &rx, &tx);
        if (rc != 0) return (uint64_t)(int64_t)rc;

        /* The caller's descriptor stops being a bare socket and becomes
         * a connection. The references were taken by sock64_connect. */
        fds[a1].rx = rx;
        fds[a1].tx = tx;

        /* A server parked in accept is waiting on exactly this. */
        sched64_wake(sock64_wait_key(listener), SCHED64_MAX_TASKS);
        return 0;
    }

    /* accept4(fd, addr, addrlen, flags), and accept(2) which is the same
     * call with no flags. Blocks when the queue is empty, and is
     * restarted rather than resumed for the same reason a pipe read is:
     * the descriptor it must return was not known when the caller
     * blocked. */
    case SYS64_ACCEPT:
    case SYS64_ACCEPT4: {
        int rx = -1, tx = -1, nfd, rc, flags;
        registers64_t self, next;
        vmspace64_t next_space;
        uint64_t next_fs;

        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (fds[a1].kind != FD64_SOCKET || fds[a1].sock < 0)
            return (uint64_t)-88;

        flags = (nr == SYS64_ACCEPT4) ? (int)args->a4 : 0;
        rc = sock64_accept(fds[a1].sock, &rx, &tx);

        if (rc == -11) {                               /* -EAGAIN */
            if (fds[a1].nonblock) return (uint64_t)-11;

            frame_from_args(args, nr, &self);
            self.rip = args->ret_rip - 2;
            if (!sched64_block_current(&self, sock64_wait_key(fds[a1].sock),
                                       nr, &next, &next_space, &next_fs))
                return (uint64_t)-11;
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);
            sched64_resume(&next);                     /* never returns */
        }
        if (rc != 0) return (uint64_t)(int64_t)rc;

        nfd = fd_alloc(3);
        if (nfd < 0) {
            /* Nowhere to put it. The connection is already made, so the
             * ends have to be released rather than dropped - the caller
             * at the far side is entitled to find out. */
            pipe64_unref(rx, 1, 0);
            pipe64_unref(tx, 0, 1);
            return (uint64_t)-24;                      /* -EMFILE */
        }
        /* A connected socket rather than a plain pipe, because
         * setsockopt and shutdown are still asked about it. Adopted, not
         * referenced: sock64_connect already counted these. */
        fd_adopt_pipe(nfd, rx, tx, -1,
                      (flags & O_NONBLOCK) != 0, (flags & O_CLOEXEC) != 0);
        fds[nfd].kind = FD64_SOCKET;

        /* The peer of a Unix socket has no address to report, and Linux
         * says so by setting the returned length to the family alone. */
        if (a3) *(uint32_t*)a3 = 2;
        return (uint64_t)nfd;
    }

    /* --- sendmsg and recvmsg, and the descriptors they carry --------- *
     *
     * SCM_RIGHTS is how Wine passes handles. Every Windows object a
     * process holds is, underneath, a file descriptor the wineserver
     * handed it over this socket - so this is not one more syscall, it
     * is the mechanism the whole Windows side is built on.
     *
     * What travels is the descriptor, not the number. The sender names
     * one of its own; the receiver gets the thing that number referred
     * to, at a number the receiver picks. That is the first operation in
     * this tree that touches two processes' descriptor tables, and the
     * reason it can be written at all is that a descriptor here is a
     * small record - a kind, a node or a pair of pipes - rather than a
     * pointer into the owner. */
    case SYS64_SENDMSG: {
        const msghdr64_t* msg = (const msghdr64_t*)a2;
        fdpass64_t carried[8];
        int ncarried = 0;
        int64_t total = 0;

        /* Said out loud while tracing, because "bad file descriptor" is
         * the same answer to four different questions and the useful
         * one is which. */
        if (a1 >= FD_MAX || !fds[a1].used) {
            if (trace) { serial64_puts("\nNOVARIS64: [sendmsg] fd ");
                         serial64_putdec(a1);
                         serial64_puts(" is not open\n"); }
            return (uint64_t)-9;
        }
        if (!fd_is_stream(a1)) {
            if (trace) { serial64_puts("\nNOVARIS64: [sendmsg] fd ");
                         serial64_putdec(a1);
                         serial64_puts(" kind "); serial64_putdec(fds[a1].kind);
                         serial64_puts(" is not a stream\n"); }
            return (uint64_t)-88;                      /* -ENOTSOCK */
        }
        if (!msg) return (uint64_t)-14;
        if (fds[a1].tx < 0) {
            if (trace) { serial64_puts("\nNOVARIS64: [sendmsg] fd ");
                         serial64_putdec(a1);
                         serial64_puts(" has no write end (rx ");
                         serial64_putdec((uint64_t)(int64_t)fds[a1].rx);
                         serial64_puts(")\n"); }
            return (uint64_t)-9;
        }

        /* The control data first, because a failure here must not send
         * half a message: the descriptors and the bytes they describe
         * have to arrive together or not at all. */
        if (msg->msg_control && msg->msg_controllen >= sizeof(cmsghdr64_t)) {
            const uint8_t* base = (const uint8_t*)msg->msg_control;
            uint64_t off = 0;

            while (off + sizeof(cmsghdr64_t) <= msg->msg_controllen) {
                const cmsghdr64_t* c = (const cmsghdr64_t*)(base + off);
                if (c->cmsg_len < sizeof(cmsghdr64_t)) break;
                if (c->cmsg_level == SOL_SOCKET_ && c->cmsg_type == SCM_RIGHTS_) {
                    const int* sent = (const int*)(base + off + CMSG64_ALIGN(sizeof(cmsghdr64_t)));
                    uint64_t bytes = c->cmsg_len - CMSG64_ALIGN(sizeof(cmsghdr64_t));
                    int n = (int)(bytes / sizeof(int));

                    if (n > (int)(sizeof(carried)/sizeof(carried[0])))
                        return (uint64_t)-22;
                    for (int k = 0; k < n; k++) {
                        int sfd = sent[k];
                        if (sfd < 0 || sfd >= FD_MAX || !fds[sfd].used) {
                            /* The descriptor being *sent*, not the one
                             * being sent down. Distinguished because
                             * both are EBADF and Wine reports the call,
                             * so the message names the socket and says
                             * nothing about the passenger. */
                            if (trace) {
                                serial64_puts("\nNOVARIS64: [sendmsg] fd ");
                                serial64_putdec(a1);
                                serial64_puts(" cannot carry fd ");
                                serial64_putdec((uint64_t)(int64_t)sfd);
                                serial64_puts(": not open\n");
                            }
                            return (uint64_t)-9;       /* -EBADF */
                        }
                        /* A listening or unconnected socket carries
                         * state this record cannot describe, and Wine
                         * never sends one. Refused rather than sent as
                         * something subtly different. */
                        if (fds[sfd].kind == FD64_SOCKET && fds[sfd].sock >= 0)
                            return (uint64_t)-22;      /* -EINVAL */

                        carried[ncarried].kind = fds[sfd].kind;
                        carried[ncarried].node = fds[sfd].node;
                        carried[ncarried].pos  = fds[sfd].pos;
                        carried[ncarried].rx   = fds[sfd].rx;
                        carried[ncarried].tx   = fds[sfd].tx;
                        /* Referenced for the receiver now. The sender is
                         * free to close its own copy the moment this
                         * returns - and does - so a reference taken only
                         * on arrival would be taken on a pipe that had
                         * already been given back. */
                        if (fds[sfd].kind == FD64_FILE)
                            ramfs64_ref_node(fds[sfd].node);
                        else {
                            pipe64_ref(fds[sfd].rx, 1, 0);
                            pipe64_ref(fds[sfd].tx, 0, 1);
                        }
                        ncarried++;
                    }
                }
                if (c->cmsg_len == 0) break;
                off += CMSG64_ALIGN(c->cmsg_len);
            }
        }

        if (ncarried && !pipe64_send_fds(fds[a1].tx, carried, ncarried)) {
            /* Undone the same way it was done. The loop above takes a
             * node reference for a file and a pipe reference for
             * anything else, so releasing everything as a pipe leaks the
             * node and gives back ends that were never taken. */
            for (int k = 0; k < ncarried; k++) {
                if (carried[k].kind == FD64_FILE)
                    ramfs64_unref_node(carried[k].node);
                else {
                    pipe64_unref(carried[k].rx, 1, 0);
                    pipe64_unref(carried[k].tx, 0, 1);
                }
            }
            /* Worth a line whether or not tracing is on. Wine's
             * send_fd() treats anything but success as fatal, and the
             * receiving wineserver has no way to tell a descriptor that
             * was never sent from one that arrived: it records the
             * server side as -1 and answers the next init_thread with
             * STATUS_TOO_MANY_OPENED_FILES, several layers away from
             * here. */
            serial64_puts("NOVARIS64: [fdpass] queue full on fd ");
            serial64_putdec(a1);
            serial64_puts(", ");
            serial64_putdec((uint64_t)ncarried);
            serial64_puts(" descriptors refused\n");
            return (uint64_t)-11;                      /* -EAGAIN */
        }

        for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
            const iovec64_t* v = &msg->msg_iov[i];
            int64_t w;
            if (!v->iov_len) continue;
            w = do_pipe_write((int)a1, v->iov_base, v->iov_len);
            if (w < 0) return (uint64_t)w;
            total += w;
            if ((uint64_t)w < v->iov_len) break;        /* a short write */
        }
        sendmsgs++;
        return (uint64_t)total;
    }

    case SYS64_RECVMSG: {
        msghdr64_t* msg = (msghdr64_t*)a2;
        fdpass64_t got[8];
        int ngot, installed = 0;
        int64_t total = 0;

        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (!fd_is_stream(a1)) return (uint64_t)-88;
        if (!msg) return (uint64_t)-14;
        if (fds[a1].rx < 0) return (uint64_t)-9;

        /* The bytes first, and through the same path an ordinary read
         * takes - so an empty connection blocks and is restarted here
         * exactly as it is there, rather than having a second, subtly
         * different waiting rule. */
        for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
            const iovec64_t* v = &msg->msg_iov[i];
            int64_t r;
            if (!v->iov_len) continue;
            r = do_pipe_read(args, nr, (int)a1, v->iov_base, v->iov_len);
            if (r < 0) return (uint64_t)r;
            total += r;
            if (r == 0) break;                          /* end of file */
            if ((uint64_t)r < v->iov_len) break;
        }

        msg->msg_flags = 0;
        msg->msg_namelen = 0;

        ngot = pipe64_recv_fds(fds[a1].rx, got, 8);
        /* A receiver that asked for ancillary data, got bytes, and got no
         * descriptor.
         *
         * wineserver's receive_fd() reads exactly one `struct send_fd`
         * - eight bytes - and expects the descriptor those bytes
         * describe to arrive with them. When it does not, the server
         * records the server side as -1, and the next init_thread on
         * that thread answers STATUS_TOO_MANY_OPENED_FILES: the failure
         * surfaces as NtCreateUserProcess refusing to start explorer.exe,
         * with nothing in between naming a descriptor.
         *
         * So the anomaly is worth a line by itself. Silence here means
         * every receiver that wanted an fd got one, and the explorer
         * failure is somewhere else entirely. */
        if (ngot == 0 && total > 0 && msg->msg_control
                && msg->msg_controllen >= sizeof(cmsghdr64_t)) {
            serial64_puts("NOVARIS64: [fdrecv] pid ");
            serial64_putdec((uint64_t)proc64_current_pid());
            serial64_puts(" fd ");
            serial64_putdec(a1);
            serial64_puts(": ");
            serial64_putdec((uint64_t)total);
            serial64_puts(" bytes, no descriptor\n");
        }
        if (ngot > 0 && !msg->msg_control) {
            serial64_puts("NOVARIS64: [fdrecv] pid ");
            serial64_putdec((uint64_t)proc64_current_pid());
            serial64_puts(" fd ");
            serial64_putdec(a1);
            serial64_puts(": ");
            serial64_putdec((uint64_t)ngot);
            serial64_puts(" descriptors dropped, receiver asked for none\n");
        }
        if (ngot > 0 && msg->msg_control) {
            uint8_t* base = (uint8_t*)msg->msg_control;
            cmsghdr64_t* c = (cmsghdr64_t*)base;
            int* out = (int*)(base + CMSG64_ALIGN(sizeof(cmsghdr64_t)));
            uint64_t need = CMSG64_ALIGN(sizeof(cmsghdr64_t))
                          + (uint64_t)ngot * sizeof(int);

            if (msg->msg_controllen < need) {
                /* No room. The descriptors are already the receiver's -
                 * the sender gave them away - so they are closed rather
                 * than lost, and MSG_CTRUNC says what happened. */
                for (int k = 0; k < ngot; k++) {
                    pipe64_unref(got[k].rx, 1, 0);
                    pipe64_unref(got[k].tx, 0, 1);
                }
                msg->msg_flags = MSG_CTRUNC_;
                msg->msg_controllen = 0;
            } else {
                for (int k = 0; k < ngot; k++) {
                    int nfd = fd_alloc(3);
                    if (nfd < 0) {
                        pipe64_unref(got[k].rx, 1, 0);
                        pipe64_unref(got[k].tx, 0, 1);
                        msg->msg_flags = MSG_CTRUNC_;
                        continue;
                    }
                    /* Adopted: the reference sendmsg took on this
                     * receiver's behalf is the one being installed. */
                    fds[nfd].kind     = got[k].kind;
                    fds[nfd].node     = got[k].node;
                    fds[nfd].pos      = got[k].pos;
                    fds[nfd].rx       = got[k].rx;
                    fds[nfd].tx       = got[k].tx;
                    fds[nfd].sock     = -1;
                    fds[nfd].nonblock = 0;
                    fds[nfd].cloexec  = (a3 & MSG_CMSG_CLOEXEC_) ? 1 : 0;
                    fds[nfd].used     = 1;
                    out[installed++]  = nfd;
                }
                c->cmsg_len   = CMSG64_ALIGN(sizeof(cmsghdr64_t))
                              + (uint64_t)installed * sizeof(int);
                c->cmsg_level = SOL_SOCKET_;
                c->cmsg_type  = SCM_RIGHTS_;
                msg->msg_controllen = installed ? c->cmsg_len : 0;
            }
        } else {
            msg->msg_controllen = 0;
        }

        recvmsgs++;
        fds_passed += (uint64_t)installed;
        return (uint64_t)total;
    }

    /* shutdown(fd, how). 0 = no more reading, 1 = no more writing,
     * 2 = both. Dropping the end is the whole of it: a writer whose
     * reader has shut down gets -EPIPE, which is how the far side finds
     * out, and that is the machinery Milestone 74 already built. */
    case SYS64_SHUTDOWN: {
        int rx, tx;
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (!fd_is_stream(a1)) return (uint64_t)-88;

        rx = fds[a1].rx;
        tx = fds[a1].tx;
        if (a2 == 0 || a2 == 2) {
            if (rx >= 0) { pipe64_unref(rx, 1, 0); fds[a1].rx = -1;
                           sched64_wake(PIPE64_WAIT_KEY(rx), SCHED64_MAX_TASKS); }
        }
        if (a2 == 1 || a2 == 2) {
            if (tx >= 0) { pipe64_unref(tx, 0, 1); fds[a1].tx = -1;
                           sched64_wake(PIPE64_WAIT_KEY(tx), SCHED64_MAX_TASKS); }
        }
        return 0;
    }

    /* Socket options. Accepted and not acted on, and the reason this is
     * not the kind of lie Milestone 72 removed is that none of them
     * describes behaviour this kernel could get wrong: SO_PASSCRED asks
     * for credentials nobody checks, SO_REUSEADDR is about a binding
     * conflict that cannot arise with one machine and one prefix, and
     * the buffer sizes are advice about a 64KB ring. What would be a lie
     * is reporting a value that was never stored, so GETSOCKOPT reports
     * zero rather than inventing one. */
    case SYS64_SETSOCKOPT:
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        return 0;

    case SYS64_GETSOCKOPT:
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (args->a4 && args->a5) {
            *(int*)args->a4 = 0;
            *(uint32_t*)args->a5 = sizeof(int);
        }
        return 0;

    case SYS64_GETSOCKNAME: {
        sockaddr_un64_t* addr = (sockaddr_un64_t*)a2;
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (fds[a1].kind != FD64_SOCKET) return (uint64_t)-88;
        if (!addr) return (uint64_t)-14;
        addr->sun_family = AF_UNIX_;
        addr->sun_path[0] = 0;
        if (a3) *(uint32_t*)a3 = 2;
        return 0;
    }

    /* chmod(path, mode) and fchmod(fd, mode). Wine chmods its socket to
     * 0600 immediately after binding, having refused to start over the
     * same bits earlier - see Milestone 73. */
    case SYS64_CHMOD: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        int node;
        if (e) return (uint64_t)e;
        node = ramfs64_lookup(path);
        if (node < 0) return (uint64_t)-2;
        ramfs64_set_mode(node, (uint32_t)a2);
        return 0;
    }

    case SYS64_FCHMOD:
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (fds[a1].kind != FD64_FILE) return (uint64_t)-9;
        ramfs64_set_mode(fds[a1].node, (uint32_t)a2);
        return 0;

    /* setsid(2). The wineserver calls it to detach from the process
     * group it was spawned in, so that a signal to the shell does not
     * take the server with it. There are no sessions here and nothing
     * sends such a signal, so the honest implementation is to report the
     * caller's own pid - which is what a real setsid returns, because a
     * new session's id is the pid of the process that made it. */
    case SYS64_SETSID:
        return (uint64_t)proc64_current_pid();

    case SYS64_UMASK:
        return 0;

    /* rename(old, new), and renameat with the working directory.
     *
     * This is how a program replaces a file without anybody ever seeing
     * it half written - write a temporary, then move it into place in
     * one step. The wineserver saves its registry that way, and without
     * it wineboot writes reg30000.tmp, cannot install it, and never
     * finishes. */
    case SYS64_RENAME:
    case SYS64_RENAMEAT: {
        char oldp[PROC64_PATH_MAX], newp[PROC64_PATH_MAX];
        const char* o = (const char*)(nr == SYS64_RENAME ? a1 : a2);
        const char* n = (const char*)(nr == SYS64_RENAME ? a2 : args->a4);
        int64_t e;

        if (nr == SYS64_RENAMEAT) {
            /* Answered for the case that occurs - AT_FDCWD, or an
             * absolute path where the dirfd is ignored by definition -
             * rather than resolved against the wrong directory. */
            if ((int64_t)(int32_t)a1 != AT_FDCWD_ && o && o[0] != '/')
                return (uint64_t)-9;
            if ((int64_t)(int32_t)a3 != AT_FDCWD_ && n && n[0] != '/')
                return (uint64_t)-9;
        }
        if ((e = abs_path(o, oldp)) != 0) return (uint64_t)e;
        if ((e = abs_path(n, newp)) != 0) return (uint64_t)e;
        renames++;
        return (uint64_t)(int64_t)ramfs64_rename(oldp, newp);
    }

    case SYS64_FTRUNCATE:
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (fds[a1].kind != FD64_FILE) return (uint64_t)-22;   /* -EINVAL */
        return (uint64_t)(int64_t)ramfs64_resize(fds[a1].node, a2);

    case SYS64_TRUNCATE: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        int node;
        if (e) return (uint64_t)e;
        node = ramfs64_lookup(path);
        if (node < 0) return (uint64_t)-2;
        return (uint64_t)(int64_t)ramfs64_resize(node, a2);
    }

    /* --- time ------------------------------------------------------- *
     *
     * The epoch here is when the timer was installed, not 1970, and
     * that is written down rather than hidden: nothing reads an RTC, so
     * a plausible-looking date would be a fabrication, and a file
     * stamped with a fabricated date is worse than one stamped with an
     * obviously small number. What the wineserver needs is that time
     * advances and that two readings subtract correctly. */
    /* clock_gettime(clkid, ts).
     *
     * The distinction is not decoration. CLOCK_REALTIME is a date and
     * CLOCK_MONOTONIC is an interval, and answering both with "seconds
     * since the timer started" makes the first of them 1970 - which
     * Milestone 81 found Wine acting on. 0 is REALTIME and 8
     * REALTIME_COARSE; 1 MONOTONIC, 4 MONOTONIC_RAW, 6 MONOTONIC_COARSE
     * and 7 BOOTTIME all measure from an unspecified point, and boot is
     * one. */
    case SYS64_CLOCK_GETTIME: {
        struct { uint64_t sec, nsec; }* ts = (void*)a2;
        if (!ts) return (uint64_t)-14;
        if (a1 == 0 || a1 == 8) clock64_realtime(&ts->sec, &ts->nsec);
        else                    clock64_now(&ts->sec, &ts->nsec);
        return 0;
    }

    /* gettimeofday is a wall clock and has never been anything else. */
    case SYS64_GETTIMEOFDAY: {
        struct { uint64_t sec, usec; }* tv = (void*)a1;
        if (tv) {
            uint64_t s, ns;
            clock64_realtime(&s, &ns);
            tv->sec  = s;
            tv->usec = ns / 1000;
        }
        return 0;
    }

    /* time(NULL) - whole seconds since the epoch, off the same wall
     * clock gettimeofday answers from, so the two cannot disagree.
     * Linux lets the result be written through the pointer as well as
     * returned, and callers use both spellings.
     *
     * It must not answer 0, and that is not a style point. Wine caches
     * the DOS drive table for one second, in get_drives_info:
     *
     *     static time_t last_update;        // zero-initialised
     *     static unsigned int nb_drives;    // zero-initialised
     *     time_t now = time(NULL);
     *     if (now != last_update) { ...scan the drives...  }
     *
     * A time() that says 0 on the first call equals last_update, so the
     * scan never runs, nb_drives stays 0, and the prefix has no C:
     * drive. What that looks like is "could not load kernel32.dll,
     * status c0000135" about a file that is present, and it cost this
     * milestone five runs. Answering -ENOSYS was better than answering
     * 0, because -1 differs from 0; answering the real time is better
     * than either. */
    case SYS64_TIME: {
        uint64_t sec, ns;
        clock64_realtime(&sec, &ns);
        if (a1) *(uint64_t*)a1 = sec;
        return sec;
    }

    case SYS64_CLOCK_GETRES: {
        struct { uint64_t sec, nsec; }* ts = (void*)a2;
        if (ts) { ts->sec = 0; ts->nsec = 1000000000ull / CLOCK64_HZ; }
        return 0;
    }

    /* nanosleep(req, rem), and poll(2), which wait the same way.
     *
     * Neither can wait inside the kernel. Syscalls run with interrupts
     * off - see sched64_yield_current - so the tick that would end the
     * wait cannot arrive while the waiter is holding the CPU. So both
     * are written as: look, and if the answer is "not yet", hand the
     * CPU to somebody else and *restart the call*. The frame is rewound
     * over the two bytes of `syscall` with the number back in rax, the
     * same restart wait4 and a blocking pipe read use.
     *
     * When there is nobody else to run, the restart still happens - the
     * task returns to ring 3 and re-enters immediately, which is a busy
     * loop, but a busy loop with interrupts enabled between the
     * iterations. That is the whole point: the timer advances there and
     * nowhere else, so a deadline that is checked here can only be
     * reached by going out and coming back. */
    case SYS64_NANOSLEEP:
    case SYS64_CLOCK_NANOSLEEP: {
        const struct { uint64_t sec, nsec; }* req =
            (const void*)(nr == SYS64_NANOSLEEP ? a1 : args->a3);
        uint64_t want;

        /* Checked, because this is the call that proved it was needed. */
        if (!user_range_ok((uint64_t)req, sizeof(*req)))
            return (uint64_t)-14;                      /* -EFAULT */
        if (nr == SYS64_NANOSLEEP && a2 &&
            !user_range_ok(a2, sizeof(*req)))
            return (uint64_t)-14;

        if (wait_started()) {
            if (!wait_expired()) return wait_restart(args, nr,
                                                     wait_deadline[wait_slot()]);
            wait_done();
            if (nr == SYS64_NANOSLEEP && a2) {
                struct { uint64_t sec, nsec; }* rem = (void*)a2;
                rem->sec = rem->nsec = 0;
            }
            return 0;
        }

        if (!req) return (uint64_t)-14;
        want = req->sec * CLOCK64_HZ + req->nsec / (1000000000ull / CLOCK64_HZ);
        if (want == 0) return 0;
        return wait_restart(args, nr, clock64_ticks() + want);
    }

    /* poll(fds, nfds, timeout_ms).
     *
     * The wineserver's main loop. It configures epoll first, gets
     * -ENOSYS, and falls back to this - which is the older interface and
     * the easier one to answer honestly, because it asks the question
     * fresh every time instead of keeping a set in the kernel.
     *
     * Only the three events that mean anything here: readable, writable,
     * and hung up. POLLIN on a stream is "there is a byte, or there will
     * never be one again" - end of file has to report readable, or a
     * server waiting on a client that has gone never notices. */
    case SYS64_POLL: {
        struct pollfd64 { int fd; short events; short revents; }* pfds =
            (void*)a1;
        uint64_t n = a2;
        int64_t  timeout = (int64_t)(int32_t)a3;
        uint64_t ready = 0;

        if (n > FD_MAX * 2) return (uint64_t)-22;      /* -EINVAL */

        for (uint64_t i = 0; i < n; i++) {
            int fd = pfds[i].fd;
            short want = pfds[i].events, got = 0;

            pfds[i].revents = 0;
            if (fd < 0) continue;
            if (fd >= FD_MAX || !fds[fd].used) { pfds[i].revents = POLL64_NVAL;
                                                 ready++; continue; }

            if (fds[fd].kind == FD64_SOCKET && fds[fd].sock >= 0 &&
                sock64_state(fds[fd].sock) == SOCK64_LISTENING) {
                /* A listening socket is readable when somebody is
                 * waiting to be accepted. That is the event the whole
                 * server loop turns on. */
                if (sock64_pending(fds[fd].sock)) got |= POLL64_IN;
            } else if (fd_is_stream(fd)) {
                int rx = fds[fd].rx, tx = fds[fd].tx;
                if (rx >= 0 && (pipe64_available(rx) > 0 ||
                                pipe64_writers(rx) == 0)) got |= POLL64_IN;
                if (tx >= 0 && pipe64_space(tx) > 0)      got |= POLL64_OUT;
                if (tx >= 0 && pipe64_readers(tx) == 0)   got |= POLL64_HUP;
            } else {
                /* A file is always ready, which is what Linux says too. */
                got |= POLL64_IN | POLL64_OUT;
            }

            got &= (short)(want | POLL64_HUP | POLL64_NVAL);
            if (got) { pfds[i].revents = got; ready++; }
        }

        if (ready) { wait_done(); return ready; }
        if (timeout == 0) return 0;

        if (wait_started()) {
            if (wait_expired()) { wait_done(); return 0; }
            return wait_restart(args, nr, wait_deadline[wait_slot()]);
        }
        return wait_restart(args, nr,
                            timeout < 0 ? 0    /* forever */
                                        : clock64_ticks() + (uint64_t)timeout);
    }

    /* socketpair(domain, type, protocol, int sv[2]).
     *
     * Two pipes, crossed. Only AF_UNIX/SOCK_STREAM is answered, because
     * that is the only thing a socketpair can be that this kernel could
     * honestly serve - there is no network stack behind it, and a
     * SOCK_DGRAM that silently behaved like a stream would lose message
     * boundaries a caller was relying on.
     *
     * This is how every Wine process talks to the wineserver. The type
     * argument carries SOCK_CLOEXEC and SOCK_NONBLOCK in its high bits,
     * which are the same values as O_CLOEXEC and O_NONBLOCK. */
    case SYS64_SOCKETPAIR: {
        int* sv = (int*)args->a4;
        int type = (int)(a2 & 0xFF);
        int p, q, s0, s1;

        if (a1 != 1) return (uint64_t)-97;             /* -EAFNOSUPPORT */
        if (type != 1) return (uint64_t)-94;           /* -ESOCKTNOSUPPORT */
        if (!sv) return (uint64_t)-14;

        p = pipe64_create();
        if (p < 0) return (uint64_t)-23;
        q = pipe64_create();
        if (q < 0) { pipe64_unref(p, 0, 0); return (uint64_t)-23; }

        s0 = fd_alloc(3);
        if (s0 < 0) { pipe64_unref(p,0,0); pipe64_unref(q,0,0);
                      return (uint64_t)-24; }
        fd_init_pipe(s0, p, q, (a2 & O_NONBLOCK) != 0, (a2 & O_CLOEXEC) != 0);

        s1 = fd_alloc(3);
        if (s1 < 0) { fd_release(s0); pipe64_unref(q,0,0);
                      return (uint64_t)-24; }
        /* Crossed: what s0 writes, s1 reads. */
        fd_init_pipe(s1, q, p, (a2 & O_NONBLOCK) != 0, (a2 & O_CLOEXEC) != 0);

        sv[0] = s0;
        sv[1] = s1;
        socketpairs_made++;
        return 0;
    }

    /* fcntl(fd, cmd, arg).
     *
     * Only the commands that are really about the descriptor. The file
     * locking ones are answered as success without locking anything,
     * which is a lie of exactly the kind Milestone 72 spent its whole
     * length removing - so it is confined to the case where it is
     * harmless and said out loud: there is one process holding the
     * prefix at a time here, so an advisory lock has nobody to advise. */
    case SYS64_FCNTL: {
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        switch (a2) {
        case F_GETFD:
            return fds[a1].cloexec ? FD_CLOEXEC : 0;
        case F_SETFD:
            fds[a1].cloexec = (a3 & FD_CLOEXEC) ? 1 : 0;
            return 0;
        case F_GETFL:
            /* O_RDWR because nothing here tracks the access mode a file
             * was opened with; the flag callers actually test for is
             * O_NONBLOCK, which is tracked. */
            return (uint64_t)(O_RDWR | (fds[a1].nonblock ? O_NONBLOCK : 0));
        case F_SETFL:
            fds[a1].nonblock = (a3 & O_NONBLOCK) ? 1 : 0;
            return 0;
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int nfd = fd_alloc((int)a3);
            if (nfd < 0) return (uint64_t)-24;         /* -EMFILE */
            fds[nfd] = fds[a1];
            fds[nfd].cloexec = (a2 == F_DUPFD_CLOEXEC);
            fd_take_ref(nfd);
            return (uint64_t)nfd;
        }
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
            return 0;
        default:
            return (uint64_t)-22;                      /* -EINVAL */
        }
    }

    case SYS64_DUP: {
        int nfd;
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        nfd = fd_alloc(3);
        if (nfd < 0) return (uint64_t)-24;
        fds[nfd] = fds[a1];
        fds[nfd].cloexec = 0;                          /* dup never copies it */
        fd_take_ref(nfd);
        return (uint64_t)nfd;
    }

    /* dup2(old, new) and dup3(old, new, flags). The new descriptor is
     * closed first if it was open, which is the part that matters: this
     * is how a shell wires a pipe onto stdout, and how Wine hands a
     * socket to a child at a number it agreed in advance. */
    case SYS64_DUP2:
    case SYS64_DUP3: {
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (a2 >= FD_MAX) return (uint64_t)-9;
        if (a1 == a2) return nr == SYS64_DUP3 ? (uint64_t)-22 : a2;
        fd_release((int)a2);
        fds[a2] = fds[a1];
        fds[a2].cloexec = (nr == SYS64_DUP3 && (a3 & O_CLOEXEC)) ? 1 : 0;
        fd_take_ref(a2);
        return a2;
    }

    case SYS64_LSEEK: {
        uint64_t base;
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        switch (a3) {
        case SEEK_SET_: base = 0; break;
        case SEEK_CUR_: base = fds[a1].pos; break;
        case SEEK_END_: base = ramfs64_size(fds[a1].node); break;
        default: return (uint64_t)-22;
        }
        fds[a1].pos = base + a2;
        return fds[a1].pos;
    }

    /* access(path, mode). ld.so calls this before anything else, on
     * /etc/ld.so.preload, and treats -ENOSYS as fatal enough to stop
     * looking - so answering it is the difference between a loader that
     * searches for a library and one that gives up. */
    /* readlink(path, buf, bufsiz).
     *
     * Only /proc/self/exe, and that one earns its place: it is how a
     * program finds out where it was installed. Wine uses it to locate
     * its own lib directory, and without it Wine computes the path to
     * ntdll.so as (null) and stops - which is exactly how this came to
     * be implemented. There is no /proc here, so the answer is
     * remembered when the program is loaded.
     *
     * readlink does not NUL-terminate, and a caller that assumed it did
     * would read past what it was given. */
    case SYS64_READLINK:
        return do_readlink((const char*)a1, (char*)a2, a3);

    case SYS64_READLINKAT:
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        return do_readlink((const char*)a2, (char*)a3, args->a4);

    /* statfs(2) and fstatfs(2).
     *
     * 50,820 calls in one prefix run answered -ENOSYS. Wine asks how
     * much room a drive has every time it looks at one, and a drive
     * whose size cannot be read is one it keeps asking about.
     *
     * The layout is Linux's on x86-64, checked against the host rather
     * than written from memory: 120 bytes, f_type at 0 through f_flags
     * at 80, and TMPFS_MAGIC because that is what this filesystem most
     * nearly is - ramfs64 keeps a file's bytes in RAM, so the free space
     * it should report is the free space the frame allocator has. The
     * numbers are therefore real rather than invented: a program that
     * asks whether there is room to write gets an answer that becomes
     * false in the same way the host's does. */
    case SYS64_STATFS:
    case SYS64_FSTATFS: {
        uint8_t* out;
        uint64_t total, free_frames;

        if (nr == SYS64_STATFS) {
            char path[PROC64_PATH_MAX];
            int64_t e = abs_path((const char*)a1, path);
            if (e) return (uint64_t)e;
            if (ramfs64_lookup(path) < 0) return (uint64_t)-2;   /* -ENOENT */
            out = (uint8_t*)a2;
        } else {
            if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
            out = (uint8_t*)a2;
        }

        if (!out || !user_range_ok((uint64_t)out, 120)) return (uint64_t)-14;

        total       = pmm64_total_frames();
        free_frames = pmm64_free_frames();

        for (uint64_t i = 0; i < 120; i++) out[i] = 0;
        *(uint64_t*)(out +  0) = 0x01021994ULL;    /* f_type: TMPFS_MAGIC */
        *(uint64_t*)(out +  8) = PAGE64_SIZE;      /* f_bsize              */
        *(uint64_t*)(out + 16) = total;            /* f_blocks             */
        *(uint64_t*)(out + 24) = free_frames;      /* f_bfree              */
        *(uint64_t*)(out + 32) = free_frames;      /* f_bavail             */
        *(uint64_t*)(out + 40) = ramfs64_count();  /* f_files              */
        /* Inodes are not preallocated, so what is free is what the
         * allocator could still turn into one. */
        *(uint64_t*)(out + 48) = free_frames;      /* f_ffree              */
        *(uint64_t*)(out + 64) = RAMFS64_NAME_MAX - 1; /* f_namelen        */
        *(uint64_t*)(out + 72) = PAGE64_SIZE;      /* f_frsize             */
        return 0;
    }

    /* The extended-attribute writes and removals.
     *
     * A filesystem with no attributes cannot store one, and Linux says
     * so with -EOPNOTSUPP rather than -ENOSYS; removing one that was
     * never there is -ENODATA, the same answer the reads give. The
     * distinction is not pedantry - it is the reason these are here.
     * Wine passes an errno it does not recognise through
     * errno_to_status, and 38 is not one it maps, so every -ENOSYS
     * became a `Converting errno 38` and a failure several layers up. */
    case SYS64_SETXATTR:
    case SYS64_LSETXATTR:
    case SYS64_FSETXATTR:
        return (uint64_t)-95;                      /* -EOPNOTSUPP */

    case SYS64_REMOVEXATTR:
    case SYS64_LREMOVEXATTR:
    case SYS64_FREMOVEXATTR:
        return (uint64_t)-61;                      /* -ENODATA */

    /* sched_yield(2).
     *
     * A caller spinning on a lock calls this to let the holder run, and
     * -ENOSYS turns that into a spin that never lets go of the CPU
     * until the timer takes it away. This scheduler already knows how
     * to hand the CPU on without blocking, so the answer was there;
     * only the number was missing.
     *
     * The frame is built with rax = 0 and the return address left where
     * it is, unlike the waiting calls above it, which set rip back by
     * two so the syscall re-executes. A yield does not want restarting:
     * it wants to come back having already succeeded. */
    case SYS64_SCHED_YIELD: {
        registers64_t self, next;
        vmspace64_t next_space;
        uint64_t next_fs;

        frame_from_args(args, 0, &self);
        if (sched64_yield_current(&self, &next, &next_space, &next_fs)) {
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);
            sched64_resume(&next);                     /* never returns */
        }
        /* The only runnable task. Linux returns 0 here too - the call
         * succeeded, there was simply nobody to yield to. */
        return 0;
    }

    /* uname(2).
     *
     * Six fixed 65-byte fields, 390 bytes in all, checked against the
     * host: sysname at 0, nodename at 65, release at 130, version at
     * 195, machine at 260, domainname at 325.
     *
     * It says Linux because that is the ABI this kernel implements and
     * every caller reads the field to decide which one it is talking
     * to; answering "Novaris" would be truthful about the wrong
     * question and would send glibc and Wine down paths written for
     * nothing. The release is a version this kernel really does behave
     * like - modern enough that a caller does not go looking for
     * pre-2.6 fallbacks it would not find here.
     *
     * The nodename is the part that was costing something. Wine's RPC
     * runtime asks for the computer name before it will hand off a
     * named pipe, and -ENOSYS there became `rpcrt4_ncacn_np_handoff
     * Failed to retrieve the computer name, error 2`. */
    case SYS64_UNAME: {
        char* out = (char*)a1;
        static const char* const f[6] = {
            "Linux", "novaris", "6.1.0", "#1 SMP NovarisOS", "x86_64", "(none)"
        };
        if (!out || !user_range_ok(a1, 390)) return (uint64_t)-14;
        for (uint64_t i = 0; i < 390; i++) out[i] = 0;
        for (int k = 0; k < 6; k++) kstrlcpy(out + k * 65, f[k], 65);
        return 0;
    }

    /* getrusage(2). Zeroed rather than invented.
     *
     * This kernel does not account per-process CPU time, and a made-up
     * number is worse than a zero: a caller that divides by it, or
     * compares two samples to measure progress, gets nonsense instead
     * of an obvious nothing. The struct is 144 bytes and its first two
     * fields are the timevals every caller actually reads. */
    case SYS64_GETRUSAGE: {
        uint8_t* out = (uint8_t*)a2;
        if (!out || !user_range_ok(a2, 144)) return (uint64_t)-14;
        for (uint64_t i = 0; i < 144; i++) out[i] = 0;
        return 0;
    }

    /* setpriority(2). Accepted and ignored: this scheduler is round
     * robin and has no priorities to set, and a program that lowers its
     * own is asking to be polite rather than asking a question. */
    case SYS64_SETPRIORITY:
        return 0;

    /* sysinfo(2). The layout is Linux's on x86-64, checked against the
     * host: 112 bytes, uptime at 0, totalram at 32, mem_unit at 104.
     *
     * The memory figures are the frame allocator's, in units of a page,
     * which is what mem_unit is for - so a caller that multiplies gets
     * the real number of bytes rather than a number this kernel made
     * up. There is no swap on this machine and saying so is the honest
     * answer, not a placeholder. */
    case SYS64_SYSINFO: {
        uint8_t* out = (uint8_t*)a1;
        if (!out || !user_range_ok(a1, 112)) return (uint64_t)-14;
        for (uint64_t i = 0; i < 112; i++) out[i] = 0;
        {   /* Monotonic since boot, which is what uptime means - not
             * the wall clock, which clock64_realtime answers. */
            uint64_t sec = 0, nsec = 0;
            clock64_now(&sec, &nsec);
            *(uint64_t*)(out + 0) = sec;                     /* uptime  */
        }
        *(uint64_t*)(out +  32) = pmm64_total_frames();      /* totalram */
        *(uint64_t*)(out +  40) = pmm64_free_frames();       /* freeram  */
        *(uint16_t*)(out +  80) = (uint16_t)proc64_count();  /* procs    */
        *(uint32_t*)(out + 104) = PAGE64_SIZE;               /* mem_unit */
        return 0;
    }

    /* The affinity pair. One CPU, said plainly.
     *
     * getaffinity returns the number of bytes it wrote, which is what
     * glibc uses to size its own mask - returning 0 there makes a
     * caller believe it has no CPUs at all. setaffinity accepts any
     * mask that includes the only processor there is. */
    case SYS64_SCHED_GETAFFINITY: {
        uint8_t* out = (uint8_t*)a3;
        uint64_t len = a2 < 8 ? a2 : 8;
        if (!out || !user_range_ok(a3, len)) return (uint64_t)-14;
        if (a2 < 8) return (uint64_t)-22;                    /* -EINVAL */
        for (uint64_t i = 0; i < 8; i++) out[i] = 0;
        out[0] = 1;                                          /* CPU 0    */
        return 8;
    }

    case SYS64_SCHED_SETAFFINITY:
        return 0;

    /* prlimit64(2), which is how glibc answers getrlimit.
     *
     * wineserver asks for RLIMIT_NOFILE at startup and sizes its own
     * descriptor handling by the answer, so -ENOSYS there is not
     * harmless: it leaves the server guessing about the one number this
     * milestone spent two runs raising. The limits reported are this
     * kernel's real ones - the descriptor table, the process table -
     * rather than the large round numbers a stub would invent. */
    case SYS64_PRLIMIT64: {
        uint64_t* old = (uint64_t*)args->a4;
        uint64_t cur, max;

        switch ((int)a2) {
        case 7:  cur = max = PROC64_FD_MAX;   break;   /* RLIMIT_NOFILE */
        case 6:  cur = max = PROC64_MAX;      break;   /* RLIMIT_NPROC  */
        /* 64 pages, which is what execve builds - see STACK_PAGES in
         * do_execve. Reported rather than a round 8MB, because a caller
         * that believes the larger number and recurses will find the
         * guard page instead. */
        case 3:  cur = max = 64 * PAGE64_SIZE; break;          /* STACK */
        default: cur = max = ~0ULL;           break;   /* RLIM_INFINITY */
        }
        if (old) {
            if (!user_range_ok(args->a4, 16)) return (uint64_t)-14;
            old[0] = cur;
            old[1] = max;
        }
        /* A new limit is accepted and ignored: these are properties of
         * the kernel's tables, and a process cannot lower what it does
         * not own. Refusing would fail callers that only ever set the
         * soft limit to the hard one. */
        return 0;
    }

    /* The extended-attribute reads, which this filesystem does not have.
     *
     * -ENOSYS is the wrong refusal and it was costing something: Wine
     * converts an errno it does not recognise through errno_to_status,
     * and 38 is not one it maps. -ENODATA is what Linux answers for an
     * attribute that is not set, which is true of every attribute here,
     * and callers already handle it because it is the ordinary case on
     * a filesystem that supports attributes and simply has none. */
    case SYS64_GETXATTR:
    case SYS64_LGETXATTR:
    case SYS64_FGETXATTR:
        return (uint64_t)-61;                      /* -ENODATA */

    /* utimensat(2). Accepted, and it does not lie about more than it
     * has to: ramfs64 keeps no timestamps, so there is nothing to
     * store, and reporting failure is the worse answer of the two.
     * setupapi copies a file and then stamps it; -ENOSYS there turned
     * into 76 `Converting errno 38` and a copy error for every driver
     * .inf in the prefix. */
    case SYS64_UTIMENSAT:
        return 0;

    /* faccessat(2) and faccessat2(2), which is faccessat with flags.
     *
     * Both answer the question access(2) answers, and answer it the
     * same way: this kernel has no permission bits to test, so a path
     * that resolves is a path that is accessible. AT_FDCWD is the only
     * directory descriptor a caller uses here, and a relative path is
     * resolved against the working directory by abs_path, which is what
     * AT_FDCWD means. */
    case SYS64_FACCESSAT:
    case SYS64_FACCESSAT2: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        /* -100 is AT_FDCWD. Anything else would need the descriptor's
         * directory, which nothing asks for; refused rather than
         * silently resolved against the wrong place. */
        if ((int)(int32_t)a1 != -100) return (uint64_t)-22;   /* -EINVAL */
        e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        return ramfs64_lookup(path) >= 0 ? 0 : (uint64_t)-2;  /* -ENOENT */
    }

    case SYS64_ACCESS: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return ramfs64_lookup(path) >= 0 ? 0 : (uint64_t)-2;
    }

    case SYS64_STAT: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return do_stat(path, (void*)a2, 1);
    }

    case SYS64_LSTAT: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return do_stat(path, (void*)a2, 0);
    }

    case SYS64_NEWFSTATAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        return do_stat(path, (void*)a3,
                       !(args->a4 & AT_SYMLINK_NOFOLLOW_));
    }

    /* pread64/pwrite64(fd, buf, count, offset). A positioned read that
     * does not move the file offset - which is how ld.so reads a
     * library's program headers while keeping its place, and the first
     * thing it wanted once it could open the library at all. */
    case SYS64_PREAD64: {
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        return (uint64_t)ramfs64_read(fds[a1].node, args->a4,
                                      (void*)a2, a3);
    }

    case SYS64_PWRITE64: {
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        return (uint64_t)ramfs64_write(fds[a1].node, args->a4,
                                       (const void*)a2, a3);
    }

    /* getdents64(fd, buf, count). The one call a flat path table could
     * not have answered at all: it enumerates a directory, which needs
     * children to enumerate.
     *
     * The file offset counts entries rather than bytes, which is legal -
     * d_off is opaque to the caller and only ever fed back to lseek. */
    case SYS64_GETDENTS64: {
        uint8_t* out = (uint8_t*)a2;
        uint64_t written = 0;
        int dir;

        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        dir = fds[a1].node;
        if (!ramfs64_is_dir(dir)) return (uint64_t)-20;   /* -ENOTDIR */

        for (;;) {
            uint64_t index = fds[a1].pos;
            const char* name;
            uint64_t ino, reclen, namelen;
            uint8_t type;
            int child;

            /* "." and ".." are not stored as nodes; they are
             * synthesised here, the way a real filesystem does. */
            if (index == 0) {
                name = "."; ino = (uint64_t)dir + 1; type = 4;
            } else if (index == 1) {
                int up = ramfs64_parent(dir);
                name = ".."; ino = (uint64_t)(up < 0 ? dir : up) + 1;
                type = 4;
            } else if (ramfs64_child(dir, index - 2, &child)) {
                name = ramfs64_name(child);
                ino  = (uint64_t)child + 1;
                type = ramfs64_is_dir(child) ? 4 : 8;   /* DT_DIR/DT_REG */
            } else {
                break;                                   /* end of it */
            }

            namelen = kstrlen(name);
            /* d_ino(8) d_off(8) d_reclen(2) d_type(1) name+NUL, rounded
             * to 8 so the next record starts aligned. */
            reclen = (8 + 8 + 2 + 1 + namelen + 1 + 7) & ~7ULL;
            if (written + reclen > a3) break;

            *(uint64_t*)(out + written)      = ino;
            *(uint64_t*)(out + written + 8)  = (int64_t)index + 1;
            *(uint16_t*)(out + written + 16) = (uint16_t)reclen;
            *(uint8_t*)(out + written + 18)  = type;
            kmemcpy(out + written + 19, name, namelen);
            out[written + 19 + namelen] = 0;

            written += reclen;
            fds[a1].pos++;
        }
        return written;
    }

    case SYS64_RMDIR: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return (uint64_t)(int64_t)ramfs64_rmdir(path);
    }

    case SYS64_MKDIR: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        int node;
        if (e) return (uint64_t)e;
        node = ramfs64_create(path, 1);
        if (node < 0) return (uint64_t)-28;
        /* The mode argument, which used to be discarded. The wineserver
         * creates its socket directory 0700 and refuses to start if
         * stat says otherwise. */
        ramfs64_set_mode(node, (uint32_t)a2);
        return 0;
    }

    case SYS64_MKDIRAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        {
            int node = ramfs64_create(path, 1);
            if (node < 0) return (uint64_t)-28;
            ramfs64_set_mode(node, (uint32_t)a3);
            return 0;
        }
    }

    case SYS64_UNLINK: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return (uint64_t)(int64_t)ramfs64_unlink(path);
    }

    /* unlinkat(dirfd, path, flags). One call for two operations: with
     * AT_REMOVEDIR it is rmdir and without it is unlink. glibc's
     * remove() and rmdir() both arrive here. */
    case SYS64_UNLINKAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        return (uint64_t)(int64_t)((a3 & AT_REMOVEDIR_)
                                   ? ramfs64_rmdir(path)
                                   : ramfs64_unlink(path));
    }

    case SYS64_READ:
        return (uint64_t)fd_read_bytes(args, nr, a1, (void*)a2, a3);

    case SYS64_ECHO:
        last_arg = a1;
        return a1 + 0x1111;
    /* exit(2) ends the calling THREAD. exit_group(2) ends the process.
     *
     * Treating them as the same thing is a real divergence and it was
     * observable: userland/thread64.s, run on Linux with exit(60), hung
     * - the parent thread ended and the parked child kept the process
     * alive. Novaris passed that same program, because it had only one
     * behaviour for both. */
    case SYS64_EXIT: {
        registers64_t next;
        vmspace64_t next_space;
        uint64_t next_fs;

        thread_exits++;

        /* Whether this is a thread ending or a process ending, asked of
         * the one table that knows.
         *
         * It used to be decided by sched64_exit_current's return value,
         * whose comment said "a sibling is still runnable" - and which
         * answers a different question: whether *any* task is runnable,
         * in any process. So the last thread of a process that exited
         * while anything else in the system had work to do took the
         * thread branch, and its process was never marked exited and
         * never had its files closed.
         *
         * That is not a leak, it is a deadlock, and it is what stopped
         * a prefix run finishing. wineboot starts services.exe and
         * waits - INFINITE - for it either to signal that it is up or
         * to die. services.exe reached exit(2) cleanly at 90% of a run.
         * Its socket to the wineserver stayed open because nothing
         * closed it, so the server never saw end of file, never learned
         * the process was gone, and never woke wineboot. Measured: the
         * server polled to the end of the run without one ready
         * descriptor, and wineboot never printed the "Unexpected
         * termination of services.exe" it prints when it does hear.
         *
         * Counting this process's own tasks separates the two cases.
         * The teardown happens before the switch, while this process is
         * still the current one - close_all_files and proc64_exit both
         * ask who that is. */
        if (sched64_pid_tasks(proc64_current_pid()) <= 1) {
            proc64_t* me = proc64_current();
            int parent = me ? me->parent : 0;

            exit_code = a1;
            vfork_release(me);
            close_all_files();
            proc64_exit(proc64_current_pid(), (int)a1);

            /* The same two things exit_group does once the process is
             * recorded as gone: end the run if this was the program the
             * layer was waiting for, and wake a parent parked in wait4.
             * A process that exits by exit(2) is exactly as finished as
             * one that exits by exit_group, and was being treated as
             * though it were neither. */
            if (leader_pid >= 0 && proc64_current_pid() == leader_pid) {
                leader_exited = 1;
                enter_user_mode64_abort();             /* never returns */
            }
            if (parent) sched64_wake(PROC64_WAIT_KEY(parent),
                                     SCHED64_MAX_TASKS);
        }

        if (sched64_exit_current(&next, &next_space, &next_fs)) {
            /* Something else is runnable - a sibling of this thread, or
             * another process entirely. Either way there is no
             * returning to the caller: the thread it would return to is
             * gone. */
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);        /* its own TLS */
            sched64_resume(&next);                  /* never returns */
        }
        /* Nothing left to run at all. Falling through leaves ring 3 the
         * way exit_group does. */
        exit_code = a1;
        return a1;
    }

    case SYS64_EXIT_GROUP: {
        registers64_t next;
        vmspace64_t next_space;
        uint64_t next_fs;

        exit_code = a1;
        {
            proc64_t* me = proc64_current();
            int parent = me ? me->parent : 0;
            /* The other way a vfork child stops being what its parent is
             * waiting for: it never execs, it just dies. Released before
             * the exit is recorded, so a parent that goes straight from
             * vfork to wait4 finds the child already reapable. */
            vfork_release(me);
            close_all_files();
            proc64_exit(proc64_current_pid(), (int)a1);

            /* The program the layer was waiting for. Whatever else is
             * still running was started by it and is not what the layer
             * asked about. */
            if (leader_pid >= 0 && proc64_current_pid() == leader_pid) {
                exit_code     = a1;
                leader_exited = 1;
                enter_user_mode64_abort();             /* never returns */
            }
            /* A parent blocked in wait4 is waiting on exactly this. */
            if (parent) sched64_wake(PROC64_WAIT_KEY(parent),
                                     SCHED64_MAX_TASKS);
        }

        /* If another process is runnable, this one ending is not the
         * end of the run - a parent waiting on it has to get its turn.
         * Same machinery as thread exit; the difference is only which
         * table records the status. */
        if (sched64_exit_process(proc64_current_pid(), &next, &next_space,
                                 &next_fs)) {
            /* The address space this process was standing in, freed now
             * that nothing is standing in it.
             *
             * It never was. vmspace64_destroy had call sites on fork's
             * and execve's failure paths and none on the path a process
             * actually leaves by, so every process that ran gave back
             * its descriptors and its slot and kept its memory. One
             * program at a time, that cost nothing and the frames came
             * back when the run ended. A Wine prefix is a hundred
             * processes, and this is what ran the machine out of RAM:
             * 2GB, 524,256 frames, none free, with mmap refusing even
             * the single page of KUSER_SHARED_DATA.
             *
             * Order matters twice. The switch has to come first because
             * destroying the space you are executing in unmaps the
             * tables being walked to do it - vmspace64_destroy refuses
             * that outright - and the check has to come at all because
             * threads share their process's space, as does a vfork
             * child until it execs or dies. Only the kernel half of the
             * mapping is still needed here, and that is the half
             * free_low_half_tables leaves alone. */
            vmspace64_t dying = { vmspace64_current_phys() };
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);
            if (!sched64_space_in_use(dying.pml4_phys)) {
                uint64_t before = pmm64_free_frames();
                vmspace64_destroy(&dying);
                reclaim_report("exit", before);
            }
            sched64_resume(&next);                     /* never returns */
        }
        return a1;
    }
    default:
        /* Said out loud, because there is no strace here and the only
         * way to find out what a real program wants is to let it ask. */
        unimpl_count++;
        last_unimpl = nr;
        serial64_puts("NOVARIS64: [enosys pid ");
        serial64_putdec((uint64_t)proc64_current_pid());
        serial64_puts("] syscall ");
        serial64_putdec(nr);
        /* And where it came from, because the number alone cannot tell
         * a Linux syscall from a Windows one.
         *
         * Wine's PE stubs contain a real `syscall` instruction, taken
         * when KUSER_SHARED_DATA.SystemCall is 0, and Windows syscall
         * numbers are small: ntdll's are 0x00-0x1ff. So a stub for
         * NtSomething 0x2c arrives here indistinguishable from sendto,
         * and is reported as an unimplemented sendto with nonsense
         * arguments - one of which, in the run that prompted this, was
         * 0xc0000005, STATUS_ACCESS_VIOLATION.
         *
         * The return address settles it: PE code lives at 0x6fff...,
         * the Wine loader and libc at 0x1000..., so the caller's rip
         * says which world the number belongs to. */
        serial64_puts(" from rip=");
        serial64_puthex(args->ret_rip);
        /* And, for a call that came from PE code, the byte its stub
         * tested to get here.
         *
         * KUSER_SHARED_DATA.SystemCall at 0x7ffe0308 decides whether a
         * Wine syscall stub calls the dispatcher or executes a real
         * `syscall`. Every process maps that page from the same file
         * and - measured, not assumed - the same physical frame, so a
         * write through any mapping of it is visible to all of them.
         * Printing the byte says whether it is 0 because nobody wrote
         * it, or 1 with the stub having branched anyway, which would
         * mean the stub was read wrong rather than the memory. */
        if (args->ret_rip >= 0x00006F0000000000ULL &&
            args->ret_rip <  0x0000700000000000ULL &&
            user_range_ok(0x7ffe0308ULL, 1)) {
            serial64_puts(" SystemCall=");
            serial64_putdec(*(volatile uint8_t*)0x7ffe0308ULL);
            /* Which frame that read actually landed on.
             *
             * The byte reads 1 through this same virtual address at the
             * moment the page is mapped, and 0 here, in the same
             * process. Only two things can do that: the frame holding
             * the byte was overwritten, or this address stopped
             * resolving to that frame. Translating it says which -
             * `phys` against the 0x10a09000 the mapping reported - and
             * reading the byte a second time through the direct map,
             * which does not go through the process page tables at all,
             * says whether the two views of the same frame agree. */
            {
                uint64_t phys = 0;
                if (paging64_translate(0x7ffe0308ULL, &phys) == PAGING64_OK) {
                    serial64_puts(" phys=");
                    serial64_puthex(phys);
                    serial64_puts(" viaphys=");
                    serial64_putdec(*(volatile uint8_t*)phys64_to_virt(phys));
                } else {
                    serial64_puts(" phys=untranslatable");
                }
            }
        }
        serial64_putc('\n');
        /* Linux answers an unimplemented call with -ENOSYS, and programs
         * do check for it, so this is -38 rather than -1. */
        return (uint64_t)-38;
    }
}

uint64_t syscall64_count(void)     { return call_count; }
uint64_t syscall64_last_arg(void)  { return last_arg; }
uint64_t syscall64_exit_code(void) { return exit_code; }
