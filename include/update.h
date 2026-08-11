#ifndef UPDATE_H
#define UPDATE_H

#include <stdint.h>

/* update - downloading a new version of the OS onto the disk it boots
 * from.
 *
 * Milestone 40. Two commands and one file format.
 *
 * The manifest is plain text, one `key = value` per line, because the
 * thing parsing it is a kernel with no JSON parser and no desire for one,
 * and because a format a person can read is a format a person can
 * publish. A `#` starts a comment.
 *
 *     version = 41
 *     name    = Milestone 41
 *     kernel  = http://host/novaris.bin
 *     kernel_size = 812345
 *     kernel_sum  = 3fa10c2b        # fnv1a-32, lower case hex
 *     initrd  = http://host/initrd.img
 *     initrd_size = 49283718
 *     initrd_sum  = a10b39ff
 *
 * Every field is required except `name`. The sizes and checksums are the
 * point: an update that is not verified is a way to brick a machine over
 * the network, and this one has no signature - so the least it can do is
 * refuse to install bytes that are not the bytes the manifest describes.
 *
 * What that does *not* buy, said plainly: this authenticates the download
 * against the manifest, and nothing authenticates the manifest. Anyone
 * who can answer for the update host can serve a manifest of their own.
 * Fixing that means signatures, and signatures mean a public key in the
 * image and code to check one - which is the honest next milestone rather
 * than something to pretend is already here. Over plain HTTP with no
 * signature, `update` trusts the network.
 */

#define UPDATE_MANIFEST_MAX 4096
#define UPDATE_URL_MAX      256

typedef struct {
    uint32_t version;
    char     name[64];
    char     kernel_url[UPDATE_URL_MAX];
    uint32_t kernel_size;
    uint32_t kernel_sum;
    char     initrd_url[UPDATE_URL_MAX];
    uint32_t initrd_size;
    uint32_t initrd_sum;
    int      valid;
} update_manifest_t;

/* The version this kernel was built as. Compared against the manifest's
 * to decide whether there is anything to do. */
uint32_t update_current_version(void);
const char* update_current_name(void);

/* Fetches and parses a manifest. Returns 1 on success. */
int update_check(const char* manifest_url, update_manifest_t* out);

/* Downloads both images, verifies each against the manifest, and only
 * then writes either to /disk/boot. Returns 1 if the disk now holds a
 * complete, verified update. */
int update_apply(const update_manifest_t* manifest);

/* The fnv1a-32 this kernel checksums with, shared with `sum`. */
uint32_t update_fnv1a(const uint8_t* data, uint32_t len);

#endif /* UPDATE_H */
