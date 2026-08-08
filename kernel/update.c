/* update.c - fetch a new OS and put it where the bootloader will find it.
 *
 * See include/update.h for the manifest format and for what this does and
 * does not authenticate.
 *
 * The order of operations is the whole design. Both images are downloaded
 * and both are verified *before* either is written, because the failure
 * mode being designed against is a machine that has half an update on it:
 * a new kernel with the old initrd is a machine that does not boot, and a
 * machine that does not boot cannot be updated again.
 *
 * That costs memory - the kernel and the initrd are both held at once,
 * and the initrd is tens of megabytes - and that is the right trade on a
 * machine with 768MB and no way to be rescued remotely.
 */

#include "update.h"
#include "http.h"
#include "net.h"
#include "vfs.h"
#include "kheap.h"
#include "kstring.h"
#include "console.h"
#include "vga_text.h"
#include "desktop.h"

/* Bumped by hand, one per release. A version this OS can compare against
 * a manifest has to be a number, and a number that means something has to
 * be written down somewhere - here is somewhere. */
#define NOVARIS_VERSION 40
#define NOVARIS_NAME    "Milestone 40"

uint32_t update_current_version(void) { return NOVARIS_VERSION; }
const char* update_current_name(void) { return NOVARIS_NAME; }

uint32_t update_fnv1a(const uint8_t* data, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    return h;
}

/* --- the manifest ------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static uint32_t parse_uint(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static uint32_t parse_hex(const char* s) {
    uint32_t v = 0;
    for (;;) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

/* One `key = value` per line, `#` starts a comment, whitespace ignored
 * around both. Unknown keys are skipped rather than rejected, so a
 * manifest can grow fields an older kernel does not know about - which is
 * the only forward compatibility an updater really needs. */
static int parse_manifest(const char* text, uint32_t len, update_manifest_t* m) {
    kmemset(m, 0, sizeof(*m));

    uint32_t i = 0;
    while (i < len) {
        char key[32], value[UPDATE_URL_MAX];
        uint32_t n;

        while (i < len && (is_space(text[i]) || text[i] == '\n')) i++;
        if (i >= len) break;

        if (text[i] == '#') {
            while (i < len && text[i] != '\n') i++;
            continue;
        }

        n = 0;
        while (i < len && text[i] != '=' && text[i] != '\n' &&
               n < sizeof(key) - 1) {
            if (!is_space(text[i])) key[n++] = text[i];
            i++;
        }
        key[n] = '\0';

        if (i >= len || text[i] != '=') {           /* not a key = value */
            while (i < len && text[i] != '\n') i++;
            continue;
        }
        i++;

        while (i < len && is_space(text[i])) i++;
        n = 0;
        while (i < len && text[i] != '\n' && text[i] != '#' &&
               n < sizeof(value) - 1) {
            value[n++] = text[i++];
        }
        while (n && is_space(value[n - 1])) n--;    /* trailing space */
        value[n] = '\0';
        while (i < len && text[i] != '\n') i++;

        if (kstrcmp(key, "version") == 0)          m->version = parse_uint(value);
        else if (kstrcmp(key, "name") == 0)        kstrlcpy(m->name, value, sizeof(m->name));
        else if (kstrcmp(key, "kernel") == 0)      kstrlcpy(m->kernel_url, value, sizeof(m->kernel_url));
        else if (kstrcmp(key, "kernel_size") == 0) m->kernel_size = parse_uint(value);
        else if (kstrcmp(key, "kernel_sum") == 0)  m->kernel_sum = parse_hex(value);
        else if (kstrcmp(key, "initrd") == 0)      kstrlcpy(m->initrd_url, value, sizeof(m->initrd_url));
        else if (kstrcmp(key, "initrd_size") == 0) m->initrd_size = parse_uint(value);
        else if (kstrcmp(key, "initrd_sum") == 0)  m->initrd_sum = parse_hex(value);
    }

    m->valid = m->version != 0 && m->kernel_url[0] && m->initrd_url[0] &&
               m->kernel_size != 0 && m->initrd_size != 0;
    return m->valid;
}

int update_check(const char* manifest_url, update_manifest_t* out) {
    static uint8_t buffer[UPDATE_MANIFEST_MAX];
    http_result_t res;

    int err = http_get(manifest_url, buffer, sizeof(buffer) - 1, &res);
    if (err != HTTP_OK) {
        terminal_writestring_color("update: ", VGA_COLOR_LIGHT_RED);
        terminal_writestring(http_error_string(err));
        terminal_writestring("\n");
        return 0;
    }
    if (res.status != 200) {
        terminal_writestring_color("update: the update server answered ",
                                   VGA_COLOR_LIGHT_RED);
        char n[12];
        ku32_to_dec((uint32_t)res.status, n);
        terminal_writestring(n);
        terminal_writestring("\n");
        return 0;
    }

    buffer[res.body_len] = '\0';
    if (!parse_manifest((const char*)buffer, res.body_len, out)) {
        terminal_writestring_color("update: that is not a manifest this "
                                   "kernel understands\n", VGA_COLOR_LIGHT_RED);
        return 0;
    }
    return 1;
}

/* --- applying ----------------------------------------------------------- */

static void report(const char* what, uint32_t got, uint32_t want,
                   uint32_t sum_got, uint32_t sum_want) {
    char n[12];
    terminal_writestring("  ");
    terminal_writestring(what);
    terminal_writestring(": ");
    ku32_to_dec(got, n);
    terminal_writestring(n);
    terminal_writestring(" of ");
    ku32_to_dec(want, n);
    terminal_writestring(n);
    terminal_writestring(" bytes, fnv1a ");
    ku32_to_hex(sum_got, n, 0, 8);
    terminal_writestring(n);
    if (sum_want) {
        terminal_writestring(" (want ");
        ku32_to_hex(sum_want, n, 0, 8);
        terminal_writestring(n);
        terminal_writestring(")");
    }
    terminal_writestring("\n");
}

/* The name of whatever is downloading, so the progress callback - which
 * http.c calls with nothing but two numbers - can say which file it is. */
static const char* downloading;

/* Drawn over itself with a carriage return, so a download is one line
 * that counts up rather than a screenful of them.
 *
 * And it pumps the desktop, which is the more important half. Everything
 * here runs inside the desktop's event loop, so a minute-long download
 * holds that loop for a minute: no cursor, no repaint, nothing. Pumping
 * from here hands it back between packets. Nesting is safe and is what
 * the loop was built for - see the note above desktop_pump() - and the
 * shell refuses to start a second command while this one runs, so a
 * keystroke that arrives now cannot re-enter the updater. */
static void download_progress(uint32_t got, uint32_t expected) {
    char n[12];

    /* Kilobytes, and the percentage worked out from them too.
     *
     * Not because kilobytes read better - they do - but because
     * got * 100 overflows 32 bits at 43MB, and the file this exists for
     * is 48MB. There is no 64-bit divide to fall back on: this is a
     * freestanding kernel with no libgcc, and asking for one is a link
     * error rather than slow code. Dividing the kilobyte counts keeps
     * every intermediate under 2^32 for volumes up to 4GB. */
    uint32_t got_kb = got >> 10, want_kb = expected >> 10;

    terminal_writestring("\r  ");
    terminal_writestring(downloading ? downloading : "download");
    terminal_writestring(": ");
    ku32_to_dec(got_kb, n);
    terminal_writestring(n);
    terminal_writestring("K");

    if (expected) {
        terminal_writestring(" of ");
        ku32_to_dec(want_kb, n);
        terminal_writestring(n);
        terminal_writestring("K (");
        /* A file under a kilobyte has want_kb == 0, and is finished the
         * moment it has started. */
        ku32_to_dec(want_kb ? (got_kb * 100u) / want_kb : 100u, n);
        terminal_writestring(n);
        terminal_writestring("%)");
    }

    /* Trailing blanks: the line shrinks as the numbers do not, and
     * without them the tail of the longest line printed so far stays on
     * screen for ever. */
    terminal_writestring("      ");

    desktop_pump();
}

static uint8_t* download_verified(const char* url, uint32_t size, uint32_t sum,
                                  const char* what) {
    uint8_t* buf = (uint8_t*)kmalloc(size);
    if (!buf) {
        terminal_writestring_color("update: not enough memory for ",
                                   VGA_COLOR_LIGHT_RED);
        terminal_writestring(what);
        terminal_writestring("\n");
        return 0;
    }

    http_result_t res;
    downloading = what;
    int err = http_get_progress(url, buf, size, &res, download_progress);
    downloading = 0;
    terminal_writestring("\r");
    if (err != HTTP_OK || res.status != 200) {
        terminal_writestring_color("update: could not download ",
                                   VGA_COLOR_LIGHT_RED);
        terminal_writestring(what);
        terminal_writestring(": ");
        terminal_writestring(http_error_string(err));
        terminal_writestring("\n");
        kfree(buf);
        return 0;
    }

    uint32_t got = update_fnv1a(buf, res.body_len);
    report(what, res.body_len, size, got, sum);

    if (res.body_len != size || (sum && got != sum)) {
        terminal_writestring_color("update: that is not the file the "
                                   "manifest describes - refusing it\n",
                                   VGA_COLOR_LIGHT_RED);
        kfree(buf);
        return 0;
    }
    return buf;
}

/* Writes through the VFS, creating the directory if it is not there. */
static int write_file(const char* path, const uint8_t* data, uint32_t len) {
    const char* leaf = 0;
    vfs_node_t* dir = vfs_resolve_parent(path, &leaf);
    if (!dir) return 0;

    vfs_node_t* node = vfs_finddir(dir, leaf);
    if (!node) node = vfs_create(dir, leaf, VFS_FILE);
    if (!node) return 0;

    vfs_truncate(node, 0);
    int32_t wrote = vfs_write(node, 0, len, data);
    return wrote >= 0 && (uint32_t)wrote == len;
}

int update_apply(const update_manifest_t* m) {
    if (!m || !m->valid) return 0;

    if (!vfs_lookup("/disk")) {
        terminal_writestring_color("update: no disk to install onto.\n",
                                   VGA_COLOR_LIGHT_RED);
        terminal_writestring("An update needs somewhere that survives a "
                             "reboot; /disk is that place.\n");
        return 0;
    }

    terminal_writestring("Downloading...\n");

    /* Both, and both verified, before either is written. A machine with a
     * new kernel and an old initrd does not boot, and a machine that does
     * not boot cannot be updated again. */
    uint8_t* kernel = download_verified(m->kernel_url, m->kernel_size,
                                        m->kernel_sum, "kernel");
    if (!kernel) return 0;

    uint8_t* initrd = download_verified(m->initrd_url, m->initrd_size,
                                        m->initrd_sum, "initrd");
    if (!initrd) {
        kfree(kernel);
        return 0;
    }

    terminal_writestring("Writing to /disk/boot...\n");

    vfs_node_t* disk = vfs_lookup("/disk");
    if (!vfs_finddir(disk, "boot")) vfs_create(disk, "boot", VFS_DIRECTORY);

    int ok = write_file("/disk/boot/novaris.bin", kernel, m->kernel_size) &&
             write_file("/disk/boot/initrd.img", initrd, m->initrd_size);

    kfree(initrd);
    kfree(kernel);

    if (!ok) {
        terminal_writestring_color("update: the write failed - the disk may "
                                   "be full.\n", VGA_COLOR_LIGHT_RED);
        return 0;
    }

    /* The version marker, last. Its presence is what says the two images
     * beside it are complete: a power cut halfway through writing the
     * initrd leaves a stale marker or none, and either way the boot
     * loader's check fails safely rather than loading half a kernel. */
    char stamp[16];
    ku32_to_dec(m->version, stamp);
    uint32_t n = kstrlen(stamp);
    stamp[n++] = '\n';
    write_file("/disk/boot/version", (const uint8_t*)stamp, n);

    return 1;
}
