#ifndef _INCLUDE_NARF_DATA_H_
#define _INCLUDE_NARF_DATA_H_

#include <assert.h>
#include <stdint.h>

#define NARF_SIGNATURE 0x47534653
#define NARF_VERSION 0x00000000

#define NARF_NODE_ROOT  0x05 // (0x05)
#define NARF_NODE_EMPTY 0x0A // (0x05 << 1)
#define NARF_NODE_DIR   0x14 // (0x05 << 2)
#define NARF_NODE_FILE  0x28 // (0x05 << 3)
#define NARF_NODE_DATA  0x50 // (0x05 << 4)

#define NARF_TAIL 0xFFFFFFFF

typedef struct __attribute__((packed)) {
   uint32_t signature;   // NARF_SIGNATURE
   uint32_t version;     // NARF_VERSION
   uint8_t  node_type;   // a NARF_NODE_* identifier
   uint8_t  reserved[3]; // reserved
} NARF_NodeHeader;

static_assert(sizeof(NARF_NodeHeader) == 12, "NARF_NodeHeader wrong size");

typedef struct __attribute__((packed)) {
   NARF_NodeHeader head;
   uint32_t directory_tree;  // sector containing root of directory tree
   uint32_t free_list;       // list of free sectors
   uint32_t size;            // number of used sectors
} NARF_RootNode;

static_assert(sizeof(NARF_RootNode) == 24, "NARF_RootNode wrong size");

typedef struct __attribute__((packed)) {
   NARF_NodeHeader head;
   uint32_t parent;
   uint32_t left;
   uint32_t right;
   uint32_t children;
} NARF_TreeNode;
static_assert(sizeof(NARF_TreeNode) == 28, "NARF_TreeNode wrong size");

typedef struct __attribute__((packed)) {
   union {
      NARF_NodeHeader head;
      NARF_TreeNode tree;
   };
   uint8_t name[512 - 28];
} NARF_DirNode;
static_assert(sizeof(NARF_DirNode) == 512, "NARF_DirNode wrong size");

typedef struct __attribute__((packed)) {
   NARF_NodeHeader head;
   uint32_t parent;
   uint32_t child;
} NARF_ListNode;
static_assert(sizeof(NARF_ListNode) == 20, "NARF_ListNode wrong size");

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
