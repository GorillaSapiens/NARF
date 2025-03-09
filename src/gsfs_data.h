#ifndef _INCLUDE_GSFS_DATA_H_
#define _INCLUDE_GSFS_DATA_H_

#include <assert.h>
#include <stdint.h>

#define GSFS_SIGNATURE 0x47534653
#define GSFS_VERSION 0x00000000

#define GSFS_NODE_ROOT  0x05 // (0x05)
#define GSFS_NODE_EMPTY 0x0A // (0x05 << 1)
#define GSFS_NODE_DIR   0x14 // (0x05 << 2)
#define GSFS_NODE_FILE  0x28 // (0x05 << 3)
#define GSFS_NODE_DATA  0x50 // (0x05 << 4)

#define GSFS_TAIL 0xFFFFFFFF

typedef struct __attribute__((packed)) {
   uint32_t signature;   // GSFS_SIGNATURE
   uint32_t version;     // GSFS_VERSION
   uint8_t  node_type;   // a GSFS_NODE_* identifier
   uint8_t  reserved[3]; // reserved
} GSFS_NodeHeader;

static_assert(sizeof(GSFS_NodeHeader) == 12, "GSFS_NodeHeader wrong size");

typedef struct __attribute__((packed)) {
   GSFS_NodeHeader head;
   uint32_t directory_tree;  // sector containing root of directory tree
   uint32_t free_list;       // list of free sectors
} GSFS_RootNode;

static_assert(sizeof(GSFS_RootNode) == 20, "GSFS_RootNode wrong size");

typedef struct __attribute__((packed)) {
   GSFS_NodeHeader head;
   uint32_t parent;
   uint32_t left;
   uint32_t right;
} GSFS_TreeNode;
static_assert(sizeof(GSFS_TreeNode) == 24, "GSFS_TreeNode wrong size");

typedef struct __attribute__((packed)) {
   GSFS_NodeHeader head;
   uint32_t parent;
   uint32_t child;
} GSFS_ListNode;
static_assert(sizeof(GSFS_RootNode) == 20, "GSFS_ListNode wrong size");

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
