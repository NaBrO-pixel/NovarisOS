#include "vfs.h"

vfs_node_t* vfs_root = 0;

uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node && node->read) return node->read(node, offset, size, buffer);
    return 0;
}

vfs_node_t* vfs_readdir(vfs_node_t* node, uint32_t index) {
    if (node && (node->flags & VFS_DIRECTORY) && node->readdir) {
        return node->readdir(node, index);
    }
    return 0;
}

vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name) {
    if (node && (node->flags & VFS_DIRECTORY) && node->finddir) {
        return node->finddir(node, name);
    }
    return 0;
}
