#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf.h"
#include "narf_io.h"

#ifdef NARF_DEBUG
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#endif

#ifdef __GNUC__
#define PACKED __attribute__((packed))
#else
#define PACKED
#endif

#define SIGNATURE 0x4652414E // FRAN => NARF
#define VERSION 0x00000000

#define END INVALID_NAF // i hate typing

//! @brief The Root structure for our Not A Real Filesystem
//!
//! it is kept in memory, and flushed out with narf_sync().
//! it is intentionally small.
typedef struct PACKED {
   union {
      uint32_t signature;  // SIGNATURE
      uint8_t sigbytes[4];
   };
   uint32_t version;       // VERSION
   ByteSize sector_size;   // sector size in bytes
   Sector   total_sectors; // total size of storage in sectors
   NAF root;               // sector of root node
   NAF first;              // sector of first node in key order
   NAF last;               // sector of last node in key order
   NAF chain;              // previously allocated but free now
   Sector   count;         // count of total number of allocated NAF
   Sector   vacant;        // number of first unallocated sector
} Root;
static_assert(sizeof(Root) == 2 * sizeof(uint32_t) +
                              1 * sizeof(ByteSize) +
                              3 * sizeof(Sector) +
                              4 * sizeof(NAF), "Root wrong size");

// TODO FIX is "Header" really still an appropriate name for this?
typedef struct PACKED {
   NAF parent;      // parent NAF
   NAF left;        // left sibling NAF
   NAF right;       // right sibling NAF
   NAF prev;        // previous ordered NAF
   NAF next;        // next ordered NAF

   Sector   start;       // data start sector
   Sector   length;      // data length in sectors
   ByteSize bytes;       // data size in bytes

   char key[NARF_SECTOR_SIZE - 5 * sizeof(NAF)
                             - 2 * sizeof(Sector)
                             - 1 * sizeof(ByteSize)]; // key
} Header;
static_assert(sizeof(Header) == NARF_SECTOR_SIZE, "Header wrong size");

uint8_t buffer[NARF_SECTOR_SIZE] = { 0 };
Root root = { 0 };
Header *node = (Header *) buffer;

//! @brief Get the ideal height for our tree.
Sector max_height(void) {
   Sector ret = 1;
   Sector i = root.count;

   while (i) {
      ++ret;
      i >>= 1;
   }

   return ret;
}

//! @brief Read a NAF into our buffer
//!
//! @param naf The naf to read
//! @return true on success
static bool read_buffer(NAF naf) {
   return narf_io_read(naf, buffer);
}

//! @brief Write a NAF from buffer to disk
//!
//! @param naf The NAF to write
//! @return true on success
static bool write_buffer(NAF naf) {
// #ifdef NARF_DEBUG_INTEGRITY
// // doing this here interferes with data move!
//    assert(!naf || (node->start == naf + 1));
// #endif
   return narf_io_write(naf, buffer);
}

//! @brief Verify we're working with a valid filesystem
//!
//! determines if init() or mkfs() was called and filesystem is valid
//!
//! @return true on success
static bool verify(void) {
   if (root.signature != SIGNATURE) return false;
   if (root.version != VERSION) return false;
   if (root.sector_size != NARF_SECTOR_SIZE) return false;
   return true;
}

#ifdef NARF_DEBUG

#ifdef USE_UTF8
#define HORIZ "━"
#define VERT  "┃"
#define UPPER "┏"
#define LOWER "┗"
#define NIL   "❌"
#endif

//! @brief Helper used to pretty print the NARF tree.
//!
//! @param naf The current NAF being printed
//! @param indent The number of levels to indent
//! @param pattern Bitfield indicating tree limbs to print
static void narf_pt(NAF naf, int indent, uint32_t pattern) {
   NAF l, r;
   int i;
   char *p;
   char *arm;

   if (!verify()) return;

   if (naf != END) {
      read_buffer(naf);
      l = node->left;
      r = node->right;
      p = strdup(node->key);
      narf_pt(l, indent + 1, pattern);
   }

   for (i = 0; i < indent; i++) {
      if (pattern & (1 << i)) {
#ifndef USE_UTF8
         printf("|  ");
#else
         printf(VERT "  ");
#endif
      }
      else {
         printf("   ");
      }
   }

   if (indent) {
      if (pattern & (1 << indent)) {
#ifndef USE_UTF8
         arm = "\\-";
#else
         arm = LOWER HORIZ;
#endif
      }
      else {
#ifndef USE_UTF8
         arm = "/-";
#else
         arm = UPPER HORIZ;
#endif
      }
   }
   else {
#ifndef USE_UTF8
      arm = "==";
#else
      arm = HORIZ HORIZ;
#endif
   }

   if (naf == END) {
#ifndef USE_UTF8
      printf("%s (nil)\n", arm);
#else
      printf("%s%s\n", arm, NIL);
#endif
      return;
   }
   else {
      printf("%s %s [%d]", arm, p, naf);
      if (naf == root.first) {
         printf(" (first)");
      }
      if (naf == root.last) {
         printf(" (last)");
      }
      if (naf == root.root) {
         printf(" (root)");
      }
      if (naf == root.chain) {
         // seeing this means there's a serious problem
         printf(" (chain)");
      }
      printf("\n");
      free(p);
   }

   narf_pt(r, indent + 1, (pattern ^ (3 << (indent))) & ~1);
}

//! @see narf_debug
void print_node(NAF naf) {
   read_buffer(naf);
   printf("naf = %d => '%.*s'\n",
          naf, (int) sizeof(node->key), node->key);
   printf("tree u/l/r  = %d / %d / %d\n",
          node->parent, node->left, node->right);
   printf("list p/n    = %d / %d\n",
          node->prev, node->next);
   printf("start:len   = %d:%d (%d)\n",
          node->start, node->length, node->bytes);
}

//! @see narf_debug()
void print_chain(void) {
   NAF naf;
   if (root.chain == END) {
      printf("freechain is empty\n");
   }
   else {
      printf("freechain:\n");
      naf = root.chain;
      while (naf != END) {
         read_buffer(naf);
         printf("%d (%d:%d) -> %d\n", naf, node->start, node->length, node->next);
         naf = node->next;
      }
      printf("\n");
   }
}

//! @see narf.h
void narf_debug(void) {
   NAF naf;

   printf("root.signature     = %08x '%4s'\n", root.signature, root.sigbytes);
   if (root.signature != SIGNATURE) {
      printf("bad signature\n");
      return;
   }

   printf("root.version       = %08x\n", root.version);
   if (root.version != VERSION) {
      printf("bad version\n");
      return;
   }

   printf("root.sector_size   = %d\n", root.sector_size);
   if (root.sector_size != NARF_SECTOR_SIZE) {
      printf("bad sector size\n");
      return;
   }

   printf("root.total_sectors = %d\n", root.total_sectors);
   if (root.total_sectors < 2) {
      printf("bad total sectors\n");
      return;
   }

   printf("root.vacant        = %d\n", root.vacant);
   printf("root.chain         = %d\n", root.chain);
   printf("root.root          = %d\n", root.root);
   printf("root.first         = %d\n", root.first);
   printf("root.last          = %d\n", root.last);
   printf("root.count         = %d\n", root.count);
   printf("\n");

   naf = root.first;
   while (naf != END) {
      print_node(naf);
      printf("\n");

      naf = node->next;
   }

   print_chain();
   printf("\n");

   if (root.root == END) {
      printf("tree is empty\n");
   }
   else {
      printf("tree:\n");
      narf_pt(root.root, 0, 0);
   }
}
#endif

#ifdef NARF_DEBUG_INTEGRITY
void verify_not_on_tree(NAF parent, NAF naf) {
   NAF l, r;

   if (parent == END) {
      return;
   }
   assert(parent != naf);

   read_buffer(parent);
   l = node->left;
   r = node->right;

   verify_not_on_tree(l, naf);
   verify_not_on_tree(r, naf);
}

void verify_not_in_chain(NAF naf) {
   NAF tmp;
   tmp = root.chain;
   while (tmp != END) {
      read_buffer(tmp);
      assert(naf != tmp);
      tmp = node->next;
   }
}

//! @brief if it's in the tree, it should not be in chain
void walk_tree(NAF parent, NAF naf) {
   NAF l, r;

   if (naf == END) {
      return;
   }

   verify_not_in_chain(naf);

   // assert parent linkage
   if (parent == END) {
      assert(root.root == naf);
   }
   else {
      read_buffer(parent);
      assert((node->left == naf || node->right == naf) &&
             (node->left != node->right));
   }

   read_buffer(naf);
   l = node->left;
   r = node->right;

   // assert parent
   assert(node->parent == parent);

   walk_tree(naf, l);
   walk_tree(naf, r);
}

//! @brief if it's in the chain, it should not be in tree
void walk_chain(void) {
   NAF tmp;
   NAF next;
   tmp = root.chain;
   while (tmp != END) {
      read_buffer(tmp);
      next = node->next;
      verify_not_on_tree(root.root, tmp);
      tmp = next;
   }
}

//! @brief detect a loop in the chain linked list
void chain_loop(void) {
   NAF a, b;

   a = b = root.chain;

   while (a != END && b != END) {

      assert(a < root.vacant);

      read_buffer(a);
      a = node->next;

      read_buffer(b);
      b = node->next;
      if (b != END) {
         read_buffer(b);
         b = node->next;
      }

      if (a != END && b != END) {
         assert (a != b);
      }
   }
}

//! @brief detect a loop in the ordered linked list
void order_loop(void) {
   NAF a, b;

   a = b = root.first;

   while (a != END && b != END) {
      read_buffer(a);
      a = node->next;

      read_buffer(b);
      b = node->next;
      if (b != END) {
         read_buffer(b);
         b = node->next;
      }

      if (a != END && b != END) {
         assert (a != b);
      }
   }
}

//! @brief detect a loop in the reverse order linked list
void reverse_loop(void) {
   NAF a, b;

   a = b = root.last;

   while (a != END && b != END) {
      read_buffer(a);
      a = node->prev;

      read_buffer(b);
      b = node->next;
      if (b != END) {
         read_buffer(b);
         b = node->next;
      }

      if (a != END) {
         read_buffer(a);
         b = node->prev;
      }
      if (a != END && b != END) {
         assert (a != b);
      }
   }
}

void walk_order(void) {
   NAF prev;
   NAF next;
   NAF naf;

   if (root.first == END && root.last == END) {
      return;
   }

   assert(root.first != END && root.last != END);

   // walk forward
   prev = END;
   naf = root.first;

   while (naf != END) {
      read_buffer(naf);
      next = node->next;

      if (node->prev == END) {
         assert(root.first == naf);
      }
      else {
         read_buffer(node->prev);
         assert(node->next == naf);
      }

      prev = naf;
      naf = next;
   }

   // walk backward
   next = END;
   naf = root.last;

   while (naf != END) {
      read_buffer(naf);
      prev = node->prev;

      if (node->next == END) {
         assert(root.last == naf);
      }
      else {
         read_buffer(node->next);
         assert(node->prev == naf);
      }

      next = naf;
      naf = prev;
   }
}

void verify_integrity(void) {
   printf("verify integrity order_loop\n");
   order_loop();
   printf("verify integrity reverse_loop\n");
   reverse_loop();
   printf("verify integrity chain_loop\n");
   chain_loop();
   printf("verify integrity walk_chain\n");
   walk_chain();
   printf("verify integrity walk_tree\n");
   walk_tree(END, root.root);
   printf("verify integrity prev_next_cohesion\n");
   walk_order();
   // TODO test tree <-> first/last cohesion
}
#endif

//! @brief add a naf to the free chain
//!
//! @param naf The NAF to add
static void narf_chain(NAF naf) {
   NAF prev;
   NAF next;
   NAF tmp;
   Sector length;
   Sector tmp_length;

again:

   // reset fields
   read_buffer(naf);
   node->prev = END;
   node->next = END;
   node->left = END;
   node->right = END;
   node->parent = END;
   node->bytes = 0;
   // do NOT reset "start" and "length"
   length = node->length;
   write_buffer(naf);

#ifdef NARF_DEBUG_INTEGRITY
   printf("chaining...\n");
   print_node(naf);
#endif

   // can they be combined with another?
   prev = END;
   next = root.chain;
   while(next != END) {

      read_buffer(next);

      // anticipation...
      tmp = next;
      tmp_length = node->length;
      next = node->next;

      // are the two adjacent?
      if ((naf == tmp + tmp_length + 1) ||
          (naf + length + 1 == tmp)) {
         // remove item from chain
         if (prev == END) {
            root.chain = next;
            narf_sync();
         }
         else {
            read_buffer(prev);
            node->next = next;
            write_buffer(prev);
         }
#ifdef NARF_DEBUG_INTEGRITY
         printf("combining %d %d\n", naf, tmp);
         print_node(naf);
         print_node(tmp);
#endif
         // combine the two
         if (tmp < naf) {
            read_buffer(tmp);
            node->length += length + 1;
            write_buffer(tmp);
            naf = tmp;
         }
         else {
            read_buffer(naf);
            node->length += tmp_length + 1;
            write_buffer(naf);
         }
         // reinsert
         goto again;
      }

      prev = tmp;
      // next has already been set!
   }

   // dumbest case, can we rewind root.vacant?
   if (root.vacant == naf + length + 1) {
#ifdef NARF_DEBUG_INTEGRITY
      printf("rewind to %d\n", naf);
#endif
      root.vacant = naf;
      narf_sync();
      return;
   }

   // reset the buffer
   read_buffer(naf);

   // record them in the free chain
   if (root.chain == END) {
#ifdef NARF_DEBUG_INTEGRITY
      printf("head of chain\n");
#endif
      // done above // read_buffer(naf);
      node->next = root.chain;
      write_buffer(naf);

      root.chain = naf;
      narf_sync();
   }
   else {
      // smallest records first
      // done above // read_buffer(naf);
      length = node->length;

      prev = END;
      next = root.chain;
      read_buffer(next);

      while (length > node->length && next != END) {
         prev = next;
         next = node->next;
         if (next != END) {
            read_buffer(next);
         }
      }
      if (prev == END) {
         root.chain = naf;
         narf_sync();
      }
      else {
         read_buffer(prev);
         node->next = naf;
         write_buffer(prev);
      }
      read_buffer(naf);
      node->next = next;
      write_buffer(naf);
   }

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif
}

//! @brief Insert NAF into the tree and list.
//!
//! Forces rebalance if tree is too tall.
//!
//! @return true for success
static bool narf_insert(NAF naf, const char *key) {
   NAF tmp;
   NAF p;
   int cmp;
   int height;

   height = 0;

   if (!verify()) return false;

   if (root.root == END) {
      root.root = naf;
      root.first = naf;
      root.last = naf;
      narf_sync();
   }
   else {
      p = root.root;
      while (1) {
         read_buffer(p);
         cmp = strncmp(key, node->key, sizeof(node->key));
         if (cmp < 0) {
            if (node->left != END) {
               p = node->left;
               ++height;
            }
            else {
               // we are the new left of p
               node->left = naf;
               tmp = node->prev;
               node->prev = naf;
               write_buffer(p);

               read_buffer(naf);
               node->parent = p;
               node->prev = tmp;
               node->next = p;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->next = naf;
                  write_buffer(tmp);
               }
               else {
                  root.first = naf;
                  narf_sync();
               }

               break;
            }
         }
         else if (cmp > 0) {
            if (node->right != END) {
               p = node->right;
               ++height;
            }
            else {
               // we are the new right of p
               node->right = naf;
               tmp = node->next;
               node->next = naf;
               write_buffer(p);

               read_buffer(naf);
               node->parent = p;
               node->next = tmp;
               node->prev = p;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->prev = naf;
                  write_buffer(tmp);
               }
               else {
                  root.last = naf;
                  narf_sync();
               }

               break;
            }
         }
         else {
            // this should never happen !!!
            assert(0);
         }
      }
   }

   if (height > max_height() + 2) {
      narf_rebalance();
   }

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   return true;
}

//! @see narf.h
bool narf_mkfs(Sector sectors) {
   if (!narf_io_open()) return false;

   memset(buffer, 0, sizeof(buffer));
   root.signature     = SIGNATURE;
   root.version       = VERSION;
   root.sector_size   = NARF_SECTOR_SIZE;
   root.total_sectors = sectors;
   root.vacant        = 1;
   root.root          = END;
   root.first         = END;
   root.last          = END;
   root.chain         = END;
   root.count         = 0;
   memcpy(buffer, &root, sizeof(root));
   write_buffer(0);

#ifdef NARF_DEBUG
   printf("keysize %ld\n", sizeof(node->key));
#endif

   return true;
}

//! @see narf.h
bool narf_init(void) {
   if (!narf_io_open()) return false;

   read_buffer(0);
   memcpy(&root, buffer, sizeof(root));

#ifdef NARF_DEBUG
   printf("keysize %ld\n", sizeof(node->key));
#endif

   return verify();
}

//! @see narf.h
bool narf_sync(void) {
   if (!verify()) return false;
   memset(buffer, 0, sizeof(buffer));
   memcpy(buffer, &root, sizeof(root));
   return write_buffer(0);
}

//! @see narf.h
NAF narf_find(const char *key) {
   NAF naf = root.root;
   int cmp;

   if (!verify()) return false;

   while(1) {
      if (naf == END) {
         return naf;
      }
      read_buffer(naf);
      cmp = strncmp(key, node->key, sizeof(node->key));
      if (cmp < 0) {
         naf = node->left;
      }
      else if (cmp > 0) {
         naf = node->right;
      }
      else {
         return naf;
      }
   }
   // TODO FIX detect endless loops???
}

//! @see narf.h
NAF narf_dirfirst(const char *dirname, const char *sep) {
   NAF naf;
   int cmp;

   if (!verify()) return false;

   if (root.root == END) return END;

   naf = root.root;

   while(1) {
      read_buffer(naf);
      cmp = strncmp(dirname, node->key, sizeof(node->key));
      if (cmp < 0) {
         if (node->left != END) {
            naf = node->left;
         }
         else {
            // the current node comes AFTER us
            naf = node->prev;
            break;
         }
      }
      else if (cmp > 0) {
         if (node->right != END) {
            naf = node->right;
         }
         else {
            // the current node comes BEFORE us
            // awesome
            break;
         }
      }
      else {
         // BAZINGA !!!
         // an exact match
         return naf;
      }
   }

   return narf_dirnext(dirname, sep, naf);
}

//! @see narf.h
NAF narf_dirnext(const char *dirname, const char *sep, NAF naf) {
   uint32_t dirname_len;
   uint32_t sep_len;
   char *p;

   if (!verify()) return false;

   if (naf != END) {
      read_buffer(naf);
      naf = node->next;
   }
   else {
      naf = root.first;
   }

   if (naf == END) {
      return END;
   }

   read_buffer(naf);

   // at this point, "naf" is (probably) valid,
   // it is in the buffer,
   // and it is the first node after us.

   dirname_len = strlen(dirname);
   if (strncmp(dirname, node->key, dirname_len)) {
      // not a match at all!
      return END;
   }

   sep_len = strlen(sep);
   while (!strncmp(dirname, node->key, dirname_len)) {
      // beginning matches
      p = strstr(node->key + dirname_len, sep);
      if (p == NULL || p[sep_len] == 0) {
         // no sep, or only one sep at end
         return naf;
      }
      naf = node->next;
      if (naf != END) {
         read_buffer(naf);
      }
      else {
         break;
      }
   }

   return END;
}

//! @brief Trim a naf length down
//! @see narf_alloc
//! @see narf_realloc
//!
//! adds excess unused space to the chain.
//! this is a helper function for narf_alloc()
//! and narf_realloc()
//!
//! @param naf The NAF to trim
//! @param length The desired length
static void trim_excess(NAF naf, Sector length) {
   NAF extra;
   Sector excess;

#ifdef NARF_DEBUG_INTEGRITY
   printf("TRIMMING %d %d\n", naf, length);
   print_chain();
   printf("\n");
#endif

   read_buffer(naf);

   excess = node->length - length;
   extra = node->start + length;

   node->length = length;
   write_buffer(naf);

   read_buffer(extra);
   node->start = extra + 1;
   node->length = excess - 1;
   write_buffer(extra);

   narf_chain(extra);
}

NAF narf_unchain(Sector length) {
   NAF prev;
   NAF next;
   NAF naf;

   prev = END;
   next = root.chain;
   while(next != END) {
      read_buffer(next);
      if (node->length >= length) {

#ifdef NARF_DEBUG_INTEGRITY
         printf("NEED %d FOUND %d %d:%d\n",
                length, next, node->start, node->length);
         print_node(next);
#endif

         // this will do nicely
         naf = next;
         next = node->next;

         // pull it out
         if (prev == END) {
            root.chain = next;
            narf_sync();
         }
         else {
            read_buffer(prev);
            node->next = next;
            write_buffer(prev);
         }

         read_buffer(naf);
         if (node->length > length) {
            // we need to trim the excess.
            trim_excess(naf, length);
         }
         read_buffer(naf);

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

         return naf;
      }
      prev = next;
      next = node->next;
   }
   return END;
}

//! @see narf.h
NAF narf_alloc(const char *key, ByteSize bytes) {
   NAF naf;
   Sector length;

   length = (bytes + NARF_SECTOR_SIZE - 1) / NARF_SECTOR_SIZE;

   if (!verify()) return END;

   naf = narf_find(key);

   if (naf != END) {
      return false;
   }

   // first check if we can allocate from the chain
   naf = narf_unchain(length);

   if (naf == END) {
      // nothing on the chain was suitable
 
      if (root.vacant + length + 1 > root.total_sectors) {
         // NO ROOM!!!
         return END;
      }

      naf = root.vacant;
      ++root.vacant;
      node->start  = root.vacant;
      node->length = length;
      root.vacant += length;
      write_buffer(naf);
   }

   // reset fields except start and length
   read_buffer(naf);
   node->parent = END;
   node->left   = END;
   node->right  = END;
   node->prev    = END;
   node->next    = END;
   node->bytes  = bytes;
   strncpy(node->key, key, sizeof(node->key));
   write_buffer(naf);

#ifdef NARF_DEBUG_INTEGRITY
   printf("alloc %08x %08x %d %d\n",
         naf, node->start, node->length, node->bytes);
#endif

   ++root.count;
   narf_sync();
   narf_insert(naf, key);

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   return naf;
}

//! @brief Move a NAF, copying all data
//! @see narf_realloc
//!
//! move and relink a NAF, adjusting size and
//! copying old data.
//! this is a helper function for narf_realloc()
//!
//! @param dst The destination for the NAF
//! @param src The source for the NAF
//! @param length The desired length
//! @param bytes The desired bytes
static void narf_move(NAF dst, NAF src, Sector length, ByteSize bytes) {
   NAF prev, next, parent;
   NAF left, right;
   Sector og_start;
   Sector start;
   Sector og_length;
   Sector i;

   // move into our new home
   read_buffer(src);
   og_start = node->start;
   og_length = node->length;
   start = node->start = dst + 1;
   node->length = length;
   node->bytes = bytes;
   prev = node->prev;
   next = node->next;
   parent = node->parent;
   left = node->left;
   right = node->right;
   write_buffer(dst);

   // fix up prev
   if (prev != END) {
      read_buffer(prev);
      node->next = dst;
      write_buffer(prev);
   }
   else {
      root.first = dst;
      narf_sync();
   }

   // fix up next
   if (next != END) {
      read_buffer(next);
      node->prev = dst;
      write_buffer(next);
   }
   else {
      root.last = dst;
      narf_sync();
   }

   // fix up parent
   if (parent != END) {
      read_buffer(parent);
      if (node->left == src) {
         node->left = dst;
      }
      else if (node->right == src) {
         node->right = dst;
      }
      else {
         // this should never happen
         assert(0);
      }
      write_buffer(parent);
   }
   else {
      root.root = dst;
      narf_sync();
   }

   // fix up left
   if (left != END) {
      read_buffer(left);
      node->parent = dst;
      write_buffer(left);
   }

   // fix up right
   if (right != END) {
      read_buffer(right);
      node->parent = dst;
      write_buffer(right);
   }

   // copy the data
   for (i = 0; i < og_length; i++) {
      read_buffer(og_start + i);
      write_buffer(start + i);
   }

   // chain the old naf
   narf_chain(src);
}

//! @see narf.h
NAF narf_realloc(const char *key, ByteSize bytes) {
   NAF naf;
   NAF tmp;
   Sector length;
   Sector og_length;

   if (!verify()) return END;

   naf = narf_find(key);

   if (naf == END) {
      return narf_alloc(key, bytes);
   }

   if (bytes == 0) {
      narf_free(key);
      return END;
   }

   read_buffer(naf);
   length = (bytes + NARF_SECTOR_SIZE - 1) / NARF_SECTOR_SIZE;
   og_length = node->length;

   // do we need to change at all?
   if (og_length == length) {
      node->bytes = bytes;
      write_buffer(naf);
      return naf;
   }

   if (bytes < node->bytes) {
      // we need to shrink
      node->bytes = bytes;
      node->length = length;
      write_buffer(naf);

      node->length = og_length - length - 1;
      node->start = naf + length + 2;
      write_buffer(naf + length + 1);
      narf_chain(naf + length + 1);

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

      return naf;
   }
   else {
      // we need to grow

      // first check if we can allocate from the chain
      tmp = narf_unchain(length);
      if (tmp != END) {
         // move into our new home
         narf_move(tmp, naf, length, bytes);

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

         // return the new
         return tmp;
      }

      // all options exhausted, move into vacant

      if (root.vacant + length + 1 > root.total_sectors) {
         // NO ROOM!!!
         return END;
      }

      tmp = root.vacant;
      root.vacant += length + 1;
      narf_sync();
      narf_move(tmp, naf, length, bytes);

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

      return tmp;
   }

   // this should never happen
   assert(0);
}

#ifdef NARF_SMART_FREE
//! @brief A helper function used by narf_free()
//! @see narf_free()
//!
//! replaces references to naf in parent with child
//!
//! @param parent The parent of naf
//! @param naf The naf we're skipping
//! @param child The new child
static void skip_naf(NAF parent, NAF naf, NAF child) {
   if (child != END) {
      read_buffer(child);
      node->parent = parent;
      write_buffer(child);
   }

   if (parent != END) {
      read_buffer(parent);
      if (node->left == naf) {
         node->left = child;
      }
      else if (node->right == naf) {
         node->right = child;
      }
      else {
         // this should never happen
         assert(0);
      }
      write_buffer(parent);
   }
   else {
      root.root = child;
      narf_sync();
   }

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif
}
#endif // NARF_SMART_FREE

//! @see narf.h
bool narf_free(const char *key) {
   NAF naf;
   NAF prev;
   NAF next;
#ifdef NARF_SMART_FREE
   NAF left;
   NAF right;
   NAF beta;
#endif // NARF_SMART_FREE

   if (!verify()) return false;

   naf = narf_find(key);

   if (naf == END) {
      return false;
   }

   // unlink from list
   read_buffer(naf);
   prev = node->prev;
   next = node->next;
#ifdef NARF_SMART_FREE
   left = node->left;
   right = node->right;
#endif // !NARF_SMART_FREE
   node->prev = END;
   node->next = END;
   write_buffer(naf);

   if (next != END) {
      read_buffer(next);
      node->prev = prev;
      write_buffer(next);
   }
   else {
      root.last = prev;
      narf_sync();
   }

   if (prev != END) {
      read_buffer(prev);
      node->next = next;
      write_buffer(prev);
   }
   else {
      root.first = next;
      narf_sync();
   }

#ifdef NARF_SMART_FREE
   // remove from tree

   read_buffer(naf);
   left = node->left;
   right = node->right;
   prev = node->parent; // note reuse of variable

   while (left != END && right != END) {
      // wobble down the chain until we have a free sibling
      if (prev != END) {
         read_buffer(prev);
      }
      else {
         // well this is awkward!
         // pick random direction...
         if (naf & 1) {
            node->right = naf;
         }
         else {
            node->left = naf;
         }
      }

      if (node->left == naf) {
         if (prev != END) {
            node->left = left;
            write_buffer(prev);
         }
         else {
            root.root = left;
            narf_sync();
         }

         read_buffer(left);
         beta = node->right;
         node->right = naf;
         node->parent = prev;
         write_buffer(left);

         read_buffer(naf);
         prev = node->parent = left;
         left = node->left = beta;
         write_buffer(naf);
      }
      else if (node->right == naf) {
         if (prev != END) {
            node->right = right;
            write_buffer(prev);
         }
         else {
            root.root = right;
            narf_sync();
         }

         read_buffer(right);
         beta = node->left;
         node->left = naf;
         node->parent = prev;
         write_buffer(right);

         read_buffer(naf);
         prev = node->parent = right;
         right = node->right = beta;
         write_buffer(naf);
      }
      else {
         // this should never happen
         assert(0);
      }

      read_buffer(naf);
      left = node->left;
      right = node->right;
      prev = node->parent; // note reuse of variable
   }

   if (left != END && right == END) {
      skip_naf(prev, naf, left);
   }
   else if (left == END && right != END) {
      skip_naf(prev, naf, right);
   }
   else if (left == END && right == END) {
      skip_naf(prev, naf, END);
   }

   --root.count;
   narf_sync();

   // add to the free chain
   narf_chain(naf);

#else
   --root.count;
   narf_sync();
   narf_chain(naf);
   narf_rebalance();
#endif // NARF_SMART_FREE

   return true;
}

//! @see narf.h
bool narf_rebalance(void) {
   static char key[sizeof(((Header *) 0)->key)]; // EXPENSIVE !!!
   NAF head = root.first;

   NAF naf = root.first;
   Sector count = 0;
   Sector target = 0;
   Sector spot = 0;

   Sector numerator;
   Sector denominator = 2;

   NAF prev;
   NAF next;

   if (!verify()) return false;

   while (naf != END) {
      ++count;
      read_buffer(naf);
      naf = node->next;
   }

   root.root = END;
   root.first = END;
   root.last = END;
   narf_sync();

   while (denominator < count) {
      // odd multiples of denominator
      naf = head;
      numerator = 1;
      target = count * numerator / denominator;
      spot = 0;

      while (numerator < denominator && naf != END) {
         read_buffer(naf);
         while (naf != END) {
            next = node->next;
            if (spot == target) {
               prev = node->prev;

               if (head == naf) {
                  head = next;
               }
               if (prev != END) {
                  read_buffer(prev);
                  node->next = next;
                  write_buffer(prev);
               }
               if (next != END) {
                  read_buffer(next);
                  node->prev = prev;
                  write_buffer(next);
               }

               read_buffer(naf);
               node->prev = END;
               node->next = END;
               node->left = END;
               node->right = END;
               node->parent = END;
               strncpy(key, node->key, sizeof(node->key));
               write_buffer(naf);

               narf_insert(naf, key);

               numerator += 2;
               target = count * numerator / denominator;
            }
            ++spot;
            naf = next;
            if (naf != END) {
               read_buffer(naf);
            }
         }
      }

      count = count - denominator / 2;
      denominator *= 2;
   }

   // now finish the job
   naf = head;
   while (naf != END) {
      read_buffer(naf);
      next = node->next;
      node->prev = END;
      node->next = END;
      node->left = END;
      node->right = END;
      node->parent = END;
      strncpy(key, node->key, sizeof(node->key));
      write_buffer(naf);

      narf_insert(naf, key);

      naf = next;
   }

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   return true;
}

//! @see narf.h
bool narf_defrag(void) {
   NAF tmp;
   NAF other;
   NAF parent, left, right, prev, next;
   Sector tmp_length;
   Sector other_length;
   Sector i;

   while (root.chain != END) {
      tmp = root.chain;
      read_buffer(tmp);
      root.chain = node->next;
      tmp_length = node->length;
      narf_sync();

      other = tmp + tmp_length + 1;
      read_buffer(other);
      other_length = node->length;
      parent = node->parent;
      left = node->left;
      right = node->right;
      prev = node->prev;
      next = node->next;
      node->start = tmp + 1;
      write_buffer(tmp);

      for (i = 0; i < other_length; ++i) {
         read_buffer(other + i + 1);
         write_buffer(tmp + i + 1);
      }

      if (parent != END) {
         read_buffer(parent);
         if (node->left == other) {
            node->left = tmp;
         }
         else if (node->right == other) {
            node->right = tmp;
         }
         else {
            // this should never happen
            assert(0);
         }
         write_buffer(parent);
      }
      else {
         root.root = tmp;
         narf_sync();
      }

      if (left != END) {
         read_buffer(left);
         node->parent = tmp;
         write_buffer(left);
      }

      if (right != END) {
         read_buffer(right);
         node->parent = tmp;
         write_buffer(right);
      }

      if (prev != END) {
         read_buffer(prev);
         node->next = tmp;
         write_buffer(prev);
      }
      else {
         root.first = tmp;
         narf_sync();
      }

      if (next != END) {
         read_buffer(next);
         node->prev = tmp;
         write_buffer(next);
      }
      else {
         root.last = tmp;
         narf_sync();
      }

      other = tmp + other_length + 1;

      read_buffer(other);
      node->start = other + 1;
      node->length = tmp_length;
      write_buffer(other);

      narf_chain(other);
   }

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   return true;
}

//! @see narf.h
const char *narf_key(NAF naf) {
   if (!verify() || naf == END) return NULL;
   read_buffer(naf);
   return node->key;
}

//! @see narf.h
Sector narf_sector(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->start;
}

//! @see narf.h
ByteSize narf_size(NAF naf) {
   if (!verify() || naf == END) return 0;
   read_buffer(naf);
   return node->bytes;
}

//! @see narf.h
NAF narf_first(void) {
   if (!verify()) return END;
   return root.first;
}

//! @see narf.h
NAF narf_next(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->next;
}

//! @see narf.h
NAF narf_last(void) {
   if (!verify()) return END;
   return root.last;
}

//! @see narf.h
NAF narf_previous(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->prev;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
