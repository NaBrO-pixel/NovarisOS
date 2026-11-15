/*
 * winenovaris.drv - /dev/wm, or a stand-in for it on a machine without one.
 *
 * Copyright 2026 the Novaris project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 *
 * Milestone 113. Every question about a Windows program used to cost a
 * boot of NovarisOS under emulation: a quarter of an hour to learn that a
 * DLL was missing. Most such questions are about Wine and the program,
 * not the kernel, and the build machine's own Wine answers those in
 * seconds - except that it had no display driver, so anything that made
 * a window stopped at "no driver could be loaded".
 *
 * With NOVARIS_OFFSCREEN=<directory> set and no /dev/wm, the driver is
 * its own window device: each window is a memfd, the ioctls are answered
 * here, and the process's windows are composited - in the order they
 * were stacked, at the places they were put - into
 * <directory>/screen-<pid>.ppm whenever one is damaged, a few times a
 * second at most, with <directory>/windows-<pid>.txt listing them. There
 * is no input. On NovarisOS none of this is reached: /dev/wm opens.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "novarisdrv.h"

#define OFF_MAX_FD 1024

struct off_window
{
    int used, shown, listener, attached;
    char path[300];
    int x, y, w, h;
    unsigned int order, damages;
    UINT64 id;
    unsigned int *pixels;
    size_t bytes;
    char title[WM_DEV_TITLE_MAX];
};

static struct off_window windows[OFF_MAX_FD];
static pthread_mutex_t off_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char *off_dir;
static int off_checked;
static unsigned int off_order;
static int off_w = 1024, off_h = 768;

static BOOL offscreen(void)
{
    if (!off_checked)
    {
        const char *size = getenv("NOVARIS_OFFSCREEN_SIZE");
        off_dir = getenv("NOVARIS_OFFSCREEN");
        if (size) sscanf(size, "%dx%d", &off_w, &off_h);
        off_checked = 1;
    }
    return off_dir != NULL;
}

BOOL novaris_wm_offscreen(void)
{
    int fd;

    if (!offscreen()) return FALSE;
    if ((fd = open(WMDEV_PATH, O_RDWR)) < 0) return TRUE;
    close(fd);
    return FALSE;
}

int novaris_wm_open(void)
{
    int fd = open(WMDEV_PATH, O_RDWR);

    if (fd >= 0 || !offscreen()) return fd;
    if ((fd = memfd_create("novaris-window", MFD_CLOEXEC)) < 0) return -1;
    if (fd >= OFF_MAX_FD)
    {
        close(fd);
        return -1;
    }
    pthread_mutex_lock(&off_mutex);
    memset(&windows[fd], 0, sizeof(windows[fd]));
    windows[fd].used = 1;
    pthread_mutex_unlock(&off_mutex);
    return fd;
}

void novaris_wm_close(int fd)
{
    if (fd >= 0 && fd < OFF_MAX_FD)
    {
        pthread_mutex_lock(&off_mutex);
        if (windows[fd].used)
        {
            if (windows[fd].pixels) munmap(windows[fd].pixels, windows[fd].bytes);
            if (!windows[fd].attached && windows[fd].path[0]) unlink(windows[fd].path);
            windows[fd].used = 0;
        }
        pthread_mutex_unlock(&off_mutex);
    }
    close(fd);
}

static int off_dirty;

static void write_ppm(const unsigned int *screen, const char *name)
{
    char path[512], tmp[540];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s-%d.ppm", off_dir, name, (int)getpid());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) return;
    fprintf(f, "P6\n%d %d\n255\n", off_w, off_h);
    for (int i = 0; i < off_w * off_h; i++)
    {
        unsigned char rgb[3] = { screen[i] >> 16, screen[i] >> 8, screen[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    rename(tmp, path);
}

/* Everything this process shows, bottom first, onto one picture: the
 * screen as it is, and - separately - the last screen that had any
 * window on it, because a program being ended hides its windows first
 * and the picture worth keeping is the one before. Called with
 * off_mutex held. */
static void dump(void)
{
    static unsigned int *screen;
    char path[512];
    unsigned int done = 0;
    int shown = 0;
    FILE *f;

    off_dirty = 0;
    if (!screen && !(screen = malloc((size_t)off_w * off_h * 4))) return;
    for (int i = 0; i < off_w * off_h; i++) screen[i] = 0x00305878;

    for (;;)
    {
        struct off_window *next = NULL;
        for (int i = 0; i < OFF_MAX_FD; i++)
        {
            struct off_window *w = &windows[i];
            if (!w->used || !w->shown || !w->pixels || w->order <= done) continue;
            if (!next || w->order < next->order) next = w;
        }
        if (!next) break;
        done = next->order;
        shown++;
        for (int y = 0; y < next->h; y++)
        {
            int sy = next->y + y;
            if (sy < 0 || sy >= off_h) continue;
            for (int x = 0; x < next->w; x++)
            {
                int sx = next->x + x;
                if (sx >= 0 && sx < off_w) screen[sy * off_w + sx] = next->pixels[y * next->w + x];
            }
        }
    }

    write_ppm(screen, "screen");
    if (shown) write_ppm(screen, "last-shown");
    snprintf(path, sizeof(path), "%s/windows-%d.txt", off_dir, (int)getpid());
    if ((f = fopen(path, "w")))
    {
        for (int i = 0; i < OFF_MAX_FD; i++)
        {
            struct off_window *w = &windows[i];
            if (w->used && w->pixels)
                fprintf(f, "%s %d,%d %dx%d order %u damages %u hwnd %llx \"%s\"\n", w->shown ? "shown " : "hidden",
                        w->x, w->y, w->w, w->h, w->order, w->damages, (unsigned long long)w->id, w->title);
        }
        fclose(f);
    }
}

/* Changes are written out from here, twice a second at most, so that the
 * last of a burst of draws is never the one dropped. */
static void *dump_thread(void *arg)
{
    for (;;)
    {
        struct timespec half = { 0, 500000000 };
        nanosleep(&half, NULL);
        pthread_mutex_lock(&off_mutex);
        /* Not only when this process drew: another may draw into its
         * windows (WMIO_ATTACH) and has no way to say so. */
        dump();
        pthread_mutex_unlock(&off_mutex);
    }
    return NULL;
}

static void mark_dirty(void)
{
    static int started;
    pthread_t thread;

    off_dirty = 1;
    if (!started && !pthread_create(&thread, NULL, dump_thread, NULL))
    {
        pthread_detach(thread);
        started = 1;
    }
}

int novaris_wm_ioctl(int fd, unsigned int request, void *arg)
{
    struct off_window *w;
    int ret = 0;

    if (!off_dir || fd < 0 || fd >= OFF_MAX_FD || !windows[fd].used) return ioctl(fd, request, arg);

    pthread_mutex_lock(&off_mutex);
    w = &windows[fd];
    switch (request)
    {
    case WMIO_SCREEN:
    {
        struct wm_rect *r = arg;
        r->x = r->y = 0;
        r->w = off_w;
        r->h = off_h;
        break;
    }
    case WMIO_LISTEN:
        w->listener = 1;
        break;
    case WMIO_CREATE:
    {
        struct wm_create *c = arg;
        size_t bytes = ((size_t)c->w * c->h * 4 + 4095) & ~(size_t)4095;
        int file;
        /* A file rather than the memfd, so that another process can name
         * it (WMIO_ATTACH): win-<hwnd>.raw once WMIO_SETID says which. */
        snprintf(w->path, sizeof(w->path), "%s/win-%d-%d.raw", off_dir, (int)getpid(), fd);
        if ((file = open(w->path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) >= 0)
        {
            dup3(file, fd, O_CLOEXEC);
            close(file);
        }
        if (w->pixels || !c->w || !c->h || ftruncate(fd, bytes) ||
            (w->pixels = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
        {
            w->pixels = NULL;
            errno = EINVAL;
            ret = -1;
            break;
        }
        w->bytes = bytes;
        w->w = c->w;
        w->h = c->h;
        w->shown = 1;
        w->order = ++off_order;
        memcpy(w->title, c->title, sizeof(w->title));
        w->title[sizeof(w->title) - 1] = 0;
        break;
    }
    case WMIO_GETINFO:
    {
        struct wm_info *info = arg;
        info->w = info->stride = w->w;
        info->h = info->cap_h = w->h;
        info->bytes = w->bytes;
        break;
    }
    case WMIO_MOVE:
    {
        struct wm_rect *r = arg;
        w->x = r->x;
        w->y = r->y;
        mark_dirty();
        break;
    }
    case WMIO_SHOW:
        w->shown = *(int *)arg != 0;
        mark_dirty();
        break;
    case WMIO_STACK:
    {
        struct wm_stack *s = arg;
        if (s->mode == WM_STACK_BOTTOM) w->order = 0;
        else w->order = ++off_order;
        break;
    }
    case WMIO_SETID:
    {
        char named[300];
        FILE *f;
        w->id = *(UINT64 *)arg;
        snprintf(named, sizeof(named), "%s/win-%llx.raw", off_dir, (unsigned long long)w->id);
        if (w->path[0] && !rename(w->path, named)) strcpy(w->path, named);
        snprintf(named, sizeof(named), "%s/win-%llx.size", off_dir, (unsigned long long)w->id);
        if ((f = fopen(named, "w")))
        {
            fprintf(f, "%d %d\n", w->w, w->h);
            fclose(f);
        }
        break;
    }
    case WMIO_ATTACH:
    {
        char path[300];
        int file, aw = 0, ah = 0;
        FILE *f;
        snprintf(path, sizeof(path), "%s/win-%llx.size", off_dir, (unsigned long long)*(UINT64 *)arg);
        if (!(f = fopen(path, "r")) || fscanf(f, "%d %d", &aw, &ah) != 2 || aw <= 0 || ah <= 0)
        {
            if (f) fclose(f);
            errno = ENOENT;
            ret = -1;
            break;
        }
        fclose(f);
        snprintf(path, sizeof(path), "%s/win-%llx.raw", off_dir, (unsigned long long)*(UINT64 *)arg);
        if ((file = open(path, O_RDWR | O_CLOEXEC)) < 0)
        {
            ret = -1;
            break;
        }
        dup3(file, fd, O_CLOEXEC);
        close(file);
        w->attached = 1;
        w->w = aw;
        w->h = ah;
        w->bytes = ((size_t)aw * ah * 4 + 4095) & ~(size_t)4095;
        break;
    }
    case WMIO_TITLE:
        memcpy(w->title, arg, sizeof(w->title));
        w->title[sizeof(w->title) - 1] = 0;
        break;
    case WMIO_DAMAGE:
        if (w->attached) break;
        w->damages++;
        mark_dirty();
        break;
    case WMIO_POLL:
    {
        struct wm_event *ev = arg;
        memset(ev, 0, sizeof(*ev));
        break;                                  /* 0: no event, ever */
    }
    default:
        errno = ENOTTY;
        ret = -1;
        break;
    }
    pthread_mutex_unlock(&off_mutex);
    return ret;
}
