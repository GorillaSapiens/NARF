#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<readline/readline.h>) && __has_include(<readline/history.h>)
#     include <readline/history.h>
#     include <readline/readline.h>
#  else
char *readline(const char *prompt);
void add_history(const char *line);
#  endif
#else
#  include <readline/history.h>
#  include <readline/readline.h>
#endif

#include <linux/limits.h>

#include "narf.h"
#include "narf_io.h"

#define ASSIGN =

extern void narf_io_configure(const char *fname);

__attribute__((weak)) 
//! @brief Weak fallback used when debug support is not linked.
void narf_debug(void) {
   printf("debug not supported\n");
}

__attribute__((weak)) 
//! @brief Weak fallback used when defrag support is not linked.
bool narf_defrag(void) {
   printf("defrag not supported\n");
   return false;
}

const char *tf[] = { "false", "true" };

void gremlins(int s, int n);

typedef void (*TesterCommandFn)(int argc, char **argv);

//! @brief Interactive tester command definition.
typedef struct {
   const char *name;
   TesterCommandFn fn;
   const char *help;
} TesterCommand;

#define TESTER_MAX_ARGS 32

static bool g_quit_requested = false;

static void cmd_alloc(int argc, char **argv);
static void cmd_append(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_create(int argc, char **argv);
static void cmd_debug(int argc, char **argv);
static void cmd_defrag(int argc, char **argv);
static void cmd_exit(int argc, char **argv);
static void cmd_findpart(int argc, char **argv);
static void cmd_format(int argc, char **argv);
static void cmd_free(int argc, char **argv);
static void cmd_fsck(int argc, char **argv);
static void cmd_gremlins(int argc, char **argv);
static void cmd_help(int argc, char **argv);
static void cmd_init(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_mbr(int argc, char **argv);
static void cmd_mkfs(int argc, char **argv);
static void cmd_mount(int argc, char **argv);
static void cmd_mvdir(int argc, char **argv);
static void cmd_pack(int argc, char **argv);
static void cmd_partition(int argc, char **argv);
static void cmd_quit(int argc, char **argv);
static void cmd_realloc(int argc, char **argv);
static void cmd_rename(int argc, char **argv);
static void cmd_scan(int argc, char **argv);
static void cmd_slurp(int argc, char **argv);
static void cmd_tag(int argc, char **argv);
static void cmd_touch(int argc, char **argv);

static void do_pack(const char *dirname);
static bool path_join(char *out, size_t out_size, const char *left, const char *right);
static bool narf_dir_key(char *out, size_t out_size, const char *parent, const char *name);
static bool pack_file(const char *host_path, const char *narf_key);
static const TesterCommand *find_command(const char *name);
static void print_help(void);
static void print_usage(const char *name);
static bool join_args(int argc, char **argv, int first, char *text, size_t text_size);
static int split_args(char *buffer, char **argv, int max_argc);
static int parse_int_arg(const char *text, int *value);
static int parse_size_arg(const char *text, NarfByteSize *value);

static const TesterCommand commands[] = {
   { "alloc", cmd_alloc,
      "alloc <key> <bytes>\n"
      "Create a new key with the requested byte size. The initial payload is zero-filled." },
   { "append", cmd_append,
      "append <key> <string>\n"
      "Append string data to an existing key. Quote strings that contain spaces." },
   { "cat", cmd_cat,
      "cat <key>\n"
      "Print a hex/ASCII dump of a key's payload." },
   { "create", cmd_create,
      "create <key> <string>\n"
      "Create a new key and initialize it with string data. Quote strings that contain spaces." },
   { "debug", cmd_debug,
      "debug\n"
      "Print internal root/data-tree/free-tree information." },
   { "defrag", cmd_defrag,
      "defrag\n"
      "Call narf_defrag() when defrag support is linked into the tester." },
   { "exit", cmd_exit,
      "exit\n"
      "Leave the tester prompt." },
   { "findpart", cmd_findpart,
      "findpart\n"
      "Search the MBR for a NARF partition and print the partition number found." },
   { "format", cmd_format,
      "format <n>\n"
      "Format the selected NARF partition." },
   { "free", cmd_free,
      "free <key>\n"
      "Delete a key and return its storage to the filesystem." },
   { "fsck", cmd_fsck,
      "fsck [deep]\n"
      "Validate NARF structure. Default is fast; 'deep' also runs slow overlap scans." },
   { "gremlins", cmd_gremlins,
      "gremlins <seed> <count>\n"
      "Run randomized tester operations. The seed makes a run reproducible." },
   { "help", cmd_help,
      "help [command]\n"
      "With no argument, list commands. With a command name, print detailed help for that command." },
   { "init", cmd_init,
      "init\n"
      "Mount/initialize a whole-image filesystem starting at sector 0." },
   { "ls", cmd_ls,
      "ls <dirname>\n"
      "List keys directly under dirname using / as the separator. Root may be listed as /." },
   { "mbr", cmd_mbr,
      "mbr [message]\n"
      "Write a classic MBR to sector 0. With a message, embed the text in the boot-code area." },
   { "mkfs", cmd_mkfs,
      "mkfs\n"
      "Format the whole image as a NARF filesystem starting at sector 0." },
   { "mount", cmd_mount,
      "mount <n>\n"
      "Mount the selected NARF partition." },
   { "mvdir", cmd_mvdir,
      "mvdir <olddir> <newdir> [<sep>]\n"
      "Rename a directory.  If sep is omitted, '/' is used." },
   { "pack", cmd_pack,
      "pack <host-directory>\n"
      "Recursively copy files from a host directory into NARF." },
   { "partition", cmd_partition,
      "partition <n>\n"
      "Create a NARF partition entry. Valid partition numbers are 1 through 4." },
   { "quit", cmd_quit,
      "quit\n"
      "Leave the tester prompt." },
   { "realloc", cmd_realloc,
      "realloc <key> <bytes>\n"
      "Resize an existing key." },
   { "rename", cmd_rename,
      "rename <old-key> <new-key>\n"
      "Rename a key." },
   { "scan", cmd_scan,
      "scan <key>\n"
      "Read and print the key's metadata area as a string." },
   { "slurp", cmd_slurp,
      "slurp <host-file>\n"
      "Read line-oriented keys from a host text file and allocate each with 1024 bytes." },
   { "tag", cmd_tag,
      "tag <key> <metadata>\n"
      "Store a metadata string in the key's metadata area. Quote metadata that contains spaces." },
   { "touch", cmd_touch,
      "touch <key>\n"
      "Create a new key with no data." },
   { NULL, NULL, NULL }
};

//! @brief Return the command table entry for a command name.
static const TesterCommand *find_command(const char *name) {
   const TesterCommand *cmd;

   if (name == NULL) {
      return NULL;
   }

   for (cmd = commands; cmd->name != NULL; ++cmd) {
      if (strcmp(cmd->name, name) == 0) {
         return cmd;
      }
   }

   return NULL;
}

#define TESTER_HELP_WIDTH 78

//! @brief Return the printable length of the first line of a help string.
static size_t help_first_line_len(const char *help) {
   const char *p;

   for (p = help; *p && *p != '\n'; ++p) {
   }

   return (size_t)(p - help);
}

//! @brief Print the first line of a help string.
static void print_help_first_line(const char *help) {
   const char *p;

   for (p = help; *p && *p != '\n'; ++p) {
      putchar(*p);
   }
}

//! @brief Print a compact tester command summary.
static void print_help(void) {
   const TesterCommand *cmd;
   size_t col = 0;

   printf("commands:\n  ");
   for (cmd = commands; cmd->name != NULL; ++cmd) {
      size_t item_len = help_first_line_len(cmd->help);

      if (col > 0) {
         if (col + 3 + item_len > TESTER_HELP_WIDTH) {
            printf("\n  ");
            col = 0;
         } else {
            printf(" | ");
            col += 3;
         }
      }

      print_help_first_line(cmd->help);
      col += item_len;
   }
   printf("\nUse 'help <command>' for details.\n");
}

//! @brief Print one command's detailed usage string.
static void print_usage(const char *name) {
   const TesterCommand *cmd = find_command(name);

   if (cmd != NULL) {
      printf("%s\n", cmd->help);
   }
}

//! @brief Join argv entries into a single space-separated text argument.
static bool join_args(int argc, char **argv, int first,
      char *text, size_t text_size) {
   size_t used = 0;

   if (text == NULL || text_size == 0) {
      return false;
   }

   text[0] = 0;

   if (first >= argc) {
      return false;
   }

   for (int i = first; i < argc; ++i) {
      size_t len = strlen(argv[i]);
      size_t extra = len + ((i == first) ? 0u : 1u);

      if (extra >= text_size - used) {
         return false;
      }

      if (i != first) {
         text[used++] = ' ';
      }

      memcpy(text + used, argv[i], len);
      used += len;
      text[used] = 0;
   }

   return true;
}

//! @brief Decode a backslash escape used by the tester command tokenizer.
static char decode_escape(char ch) {
   switch (ch) {
      case 'n':
         return '\n';
      case 'r':
         return '\r';
      case 't':
         return '\t';
      default:
         return ch;
   }
}

//! @brief Split a mutable command buffer into argc/argv style arguments.
static int split_args(char *buffer, char **argv, int max_argc) {
   char *src = buffer;
   int argc = 0;

   while (*src) {
      char *out;

      while (*src > 0 && (unsigned char)*src <= ' ') {
         ++src;
      }

      if (*src == 0) {
         break;
      }

      if (argc >= max_argc) {
         return -1;
      }

      if (*src == '"') {
         bool closed = false;

         ++src;
         argv[argc++] = src;
         out = src;

         while (*src) {
            char ch = *src++;

            if (ch == '"') {
               closed = true;
               break;
            }

            if (ch == '\\' && *src != 0) {
               ch = decode_escape(*src++);
            }

            *out++ = ch;
         }

         if (!closed) {
            return -2;
         }
      }
      else {
         argv[argc++] = src;
         out = src;

         while (*src && (unsigned char)*src > ' ') {
            char ch = *src++;

            if (ch == '\\' && *src != 0) {
               ch = decode_escape(*src++);
            }

            *out++ = ch;
         }
      }

      while (*src > 0 && (unsigned char)*src <= ' ') {
         ++src;
      }

      *out = 0;
   }

   return argc;
}

//! @brief Parse a decimal integer argument.
static int parse_int_arg(const char *text, int *value) {
   char *end = NULL;
   long parsed;

   if (text == NULL || value == NULL || *text == 0) {
      return 0;
   }

   parsed = strtol(text, &end, 0);

   if (*end != 0) {
      return 0;
   }

   *value = (int) parsed;
   return 1;
}

//! @brief Parse a non-negative byte-size argument.
static int parse_size_arg(const char *text, NarfByteSize *value) {
   char *end = NULL;
   unsigned long parsed;

   if (text == NULL || value == NULL || *text == 0 || *text == '-') {
      return 0;
   }

   parsed = strtoul(text, &end, 0);

   if (*end != 0) {
      return 0;
   }

   *value = (NarfByteSize) parsed;
   return 1;
}

//! @brief Join two host path components with exactly one slash.
static bool path_join(char *out, size_t out_size,
      const char *left, const char *right) {
   size_t left_len;
   const char *sep;
   int len;

   if (out == NULL || out_size == 0 || left == NULL || right == NULL) {
      return false;
   }

   left_len = strlen(left);
   sep = (left_len != 0 && left[left_len - 1] == '/') ? "" : "/";
   len = snprintf(out, out_size, "%s%s%s", left, sep, right);

   return len >= 0 && (size_t) len < out_size;
}

//! @brief Build a NARF directory key ending in one slash.
static bool narf_dir_key(char *out, size_t out_size, const char *parent,
      const char *name) {
   int len;

   if (out == NULL || out_size == 0 || parent == NULL || name == NULL) {
      return false;
   }

   len = snprintf(out, out_size, "%s%s/", parent, name);
   return len >= 0 && (size_t) len < out_size;
}

//! @brief Copy one host file into the mounted NARF image.
static bool pack_file(const char *host_path, const char *narf_key) {
   FILE *f;
   char data[512];
   size_t nread;
   bool ok = true;

   f = fopen(host_path, "rb");
   if (f == NULL) {
      fprintf(stderr, "pack: could not open '%s': %s\n",
            host_path, strerror(errno));
      return false;
   }

   if (narf_find(narf_key) && !narf_free(narf_key)) {
      fprintf(stderr, "pack: could not replace existing '%s'\n", narf_key);
      fclose(f);
      return false;
   }

   if (!narf_alloc(narf_key, 0)) {
      fprintf(stderr, "pack: could not create '%s'\n", narf_key);
      fclose(f);
      return false;
   }

   while ((nread = fread(data, 1, sizeof(data), f)) != 0) {
      if (!narf_append(narf_key, data, (NarfByteSize) nread)) {
         fprintf(stderr, "pack: append failed for '%s'\n", narf_key);
         ok = false;
         break;
      }
   }

   if (ferror(f)) {
      fprintf(stderr, "pack: read failed for '%s': %s\n",
            host_path, strerror(errno));
      ok = false;
   }

   fclose(f);
   return ok;
}

//! @brief Recursively pack a host directory into the mounted NARF image.
static bool do_pack_dive(const char *host_dir, const char *narf_dir, DIR *dir) {
   struct dirent *entry;
   bool ok = true;

   printf("%s -> %s\n", host_dir, narf_dir);

   while ((entry = readdir(dir)) != NULL) {
      char host_path[PATH_MAX];
      char narf_key[PATH_MAX];
      struct stat st;

      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
         continue;
      }

      if (!path_join(host_path, sizeof(host_path), host_dir, entry->d_name)) {
         fprintf(stderr, "pack: host path too long under '%s'\n", host_dir);
         ok = false;
         continue;
      }

      if (lstat(host_path, &st) != 0) {
         fprintf(stderr, "pack: could not stat '%s': %s\n",
               host_path, strerror(errno));
         ok = false;
         continue;
      }

      if (S_ISDIR(st.st_mode)) {
         DIR *child;

         if (!narf_dir_key(narf_key, sizeof(narf_key), narf_dir, entry->d_name)) {
            fprintf(stderr, "pack: NARF path too long under '%s'\n", narf_dir);
            ok = false;
            continue;
         }

         if (!narf_find(narf_key) && !narf_alloc(narf_key, 0)) {
            fprintf(stderr, "pack: could not create directory key '%s'\n", narf_key);
            ok = false;
            continue;
         }

         child = opendir(host_path);
         if (child == NULL) {
            fprintf(stderr, "pack: could not open directory '%s': %s\n",
                  host_path, strerror(errno));
            ok = false;
            continue;
         }

         if (!do_pack_dive(host_path, narf_key, child)) {
            ok = false;
         }
         closedir(child);
      }
      else if (S_ISREG(st.st_mode)) {
         int len = snprintf(narf_key, sizeof(narf_key), "%s%s",
               narf_dir, entry->d_name);

         if (len < 0 || (size_t) len >= sizeof(narf_key)) {
            fprintf(stderr, "pack: NARF path too long under '%s'\n", narf_dir);
            ok = false;
            continue;
         }

         if (!pack_file(host_path, narf_key)) {
            ok = false;
         }
      }
      else {
         fprintf(stderr, "pack: skipping non-regular '%s'\n", host_path);
      }
   }

   return ok;
}

//! @brief Pack a host directory into the mounted NARF image.
static void do_pack(const char *dirname) {
   DIR *dir = opendir(dirname);
   if (dir) {
      (void) do_pack_dive(dirname, "", dir);
      closedir(dir);
   }
   else {
      fprintf(stderr, "could not open '%s'\n", dirname);
   }
}

static void cmd_alloc(int argc, char **argv) {
   NarfByteSize size;
   bool result;

   if (argc != 3 || !parse_size_arg(argv[2], &size)) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_alloc(%s,%lu)=%s\n",
         argv[1], (unsigned long) size,
         tf[result ASSIGN narf_alloc(argv[1], size)]);
}

static void cmd_append(int argc, char **argv) {
   char data[512];
   bool result;
   NarfByteSize size;

   if (argc < 3 || !join_args(argc, argv, 2, data, sizeof(data))) {
      print_usage(argv[0]);
      return;
   }

   size = (NarfByteSize) strlen(data);
   result = narf_append(argv[1], data, size);

   printf("narf_append(%s,\"%s\",%lu)=%s\n",
         argv[1], data, (unsigned long) size, tf[result]);
}

static void cmd_cat(int argc, char **argv) {
   char key[512];
   char line[17];
   bool found;
   NarfByteSize len;
   NarfSector start;
   int addr = 0;
   int tail;

   if (argc != 2) {
      print_usage(argv[0]);
      return;
   }

   snprintf(key, sizeof(key), "%s", argv[1]);
   printf("narf_find(%s)=%s\n", key, tf[found ASSIGN narf_find(key)]);
   if (!found) return;

   start = narf_sector(key);
   len = narf_size(key);
   tail = (int)(len % 16);

   while (len > 0) {
      narf_io_read(start, key);
      for (NarfByteSize i = 0; i < len && i < 512; i++) {
         if ((i % 16) == 0) {
            printf("%04x: ", addr);
            addr += 16;
         }
         printf("%02x ", (uint8_t) key[i]);
         line[i % 16] = (key[i] >= ' ' && key[i] <= '~') ? key[i] : '.';
         line[(i % 16) + 1] = 0;
         if ((i % 16) == 15) {
            printf(" %s\n", line);
            line[0] = 0;
         }
      }
      start++;
      len -= (len > 512) ? 512 : len;
   }
   if (tail) {
      printf("%*s %s\n", 3 * (16 - tail), " ", line);
   }
   printf("\n");
}

static void cmd_create(int argc, char **argv) {
   char data[512];
   bool result;
   NarfByteSize size;

   if (argc < 3 || !join_args(argc, argv, 2, data, sizeof(data))) {
      print_usage(argv[0]);
      return;
   }

   size = (NarfByteSize) strlen(data);
   result = narf_alloc(argv[1], 0);

   if (result && size) {
      result = narf_append(argv[1], data, size);
   }

   printf("create(%s,\"%s\")=%s\n",
         argv[1], data, tf[result]);
}

static void cmd_debug(int argc, char **argv) {
   (void) argv;

   if (argc != 1) {
      print_usage("debug");
      return;
   }

   narf_debug();
}

static void cmd_defrag(int argc, char **argv) {
   bool result;
   (void) argv;

   if (argc != 1) {
      print_usage("defrag");
      return;
   }

   printf("narf_defrag()=%s\n",
         tf[result ASSIGN narf_defrag()]);
}

static void cmd_exit(int argc, char **argv) {
   if (argc != 1) {
      print_usage(argv[0]);
      return;
   }

   g_quit_requested = true;
}

static void cmd_findpart(int argc, char **argv) {
   (void) argv;

   if (argc != 1) {
      print_usage("findpart");
      return;
   }

   printf("narf_findpart()=%d\n",
         narf_findpart());
}

static void cmd_format(int argc, char **argv) {
   int part;

   if (argc != 2 || !parse_int_arg(argv[1], &part)) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_format(%d)=%s\n",
         part, tf[narf_format(part)]);
}

static void cmd_free(int argc, char **argv) {
   bool result;

   if (argc != 2) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_free_key(%s)=%s\n",
         argv[1], tf[result ASSIGN narf_free_key(argv[1])]);
}


static void cmd_fsck(int argc, char **argv) {
   NarfFsckReport report;
   bool result;
   bool deep = false;

   if (argc == 2 && strcmp(argv[1], "deep") == 0) {
      deep = true;
   }
   else if (argc != 1) {
      print_usage("fsck");
      return;
   }

   result = deep ? narf_fsck_deep(&report) : narf_fsck(&report);
   printf("%s()=%s\n", deep ? "narf_fsck_deep" : "narf_fsck", tf[result]);
   printf("  errors          = %u\n", (unsigned) report.errors);
   printf("  files           = %u\n", (unsigned) report.file_count);
   printf("  data_nodes      = %u\n", (unsigned) report.data_nodes);
   printf("  free_nodes      = %u\n", (unsigned) report.free_nodes);
   printf("  spare_nodes     = %u\n", (unsigned) report.spare_nodes);
   printf("  free_extents    = %u\n", (unsigned) report.free_extents);
   printf("  payload_sectors = %u\n", (unsigned) report.payload_sectors);
   printf("  free_sectors    = %u\n", (unsigned) report.free_sectors);
}

static void cmd_gremlins(int argc, char **argv) {
   int s;
   int n;

   if (argc != 3 || !parse_int_arg(argv[1], &s) || !parse_int_arg(argv[2], &n)) {
      print_usage(argv[0]);
      return;
   }

   gremlins(s, n);
}

static void cmd_help(int argc, char **argv) {
   const TesterCommand *cmd;

   if (argc == 1) {
      print_help();
      return;
   }

   if (argc != 2) {
      print_usage("help");
      return;
   }

   cmd = find_command(argv[1]);
   if (cmd == NULL) {
      printf("huh? Unknown command '%s'.\n", argv[1]);
      print_help();
      return;
   }

   printf("%s\n", cmd->help);
}

static void cmd_init(int argc, char **argv) {
   uint32_t start = 0;
   (void) argv;

   if (argc != 1) {
      print_usage("init");
      return;
   }

   printf("narf_init(0x%x)=%s\n",
         start, tf[narf_init(start)]);
}

static void cmd_ls(int argc, char **argv) {
   const char *entry;

   if (argc != 2) {
      print_usage(argv[0]);
      return;
   }

   printf("ls '%s' (%zu)\n", argv[1], strlen(argv[1]));

   for (entry = narf_dirfirst(argv[1], "/");
         entry != NULL;
         entry = narf_dirnext(argv[1], "/", entry)) {
      printf("%s\n", entry);
   }
   printf("(end)\n");
   printf("\n");
}

static void cmd_mbr(int argc, char **argv) {
   char message[512];

   if (argc == 1) {
      printf("narf_mbr(NULL)=%s\n",
            tf[narf_mbr(NULL)]);
      return;
   }

   if (!join_args(argc, argv, 1, message, sizeof(message))) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_mbr(%s)=%s\n",
         message, tf[narf_mbr(message)]);
}

static void cmd_mkfs(int argc, char **argv) {
   uint32_t start = 0;
   uint32_t size = narf_io_sectors();
   bool result;
   (void) argv;

   if (argc != 1) {
      print_usage("mkfs");
      return;
   }

   printf("narf_mkfs(0x%x, 0x%x)=%s\n",
         start, size, tf[result ASSIGN narf_mkfs(start, size)]);
}

static void cmd_mount(int argc, char **argv) {
   int part;

   if (argc != 2 || !parse_int_arg(argv[1], &part)) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_mount(%d)=%s\n",
         part, tf[narf_mount(part)]);
}

static void cmd_mvdir(int argc, char **argv) {
   bool result;

   if (argc != 3 && argc != 4) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_rename(%s,%s,%s)=%d\n",
         argv[1], argv[2], (argc == 4) ? argv[3] : "/", result ASSIGN narf_rename_dir(argv[1], argv[2], (argc == 4) ? argv[3] : "/"));
}
static void cmd_pack(int argc, char **argv) {
   if (argc != 2) {
      print_usage(argv[0]);
      return;
   }

   do_pack(argv[1]);
}

static void cmd_partition(int argc, char **argv) {
   int part;

   if (argc != 2 || !parse_int_arg(argv[1], &part)) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_partition(%d)=%s\n",
         part, tf[narf_partition(part)]);
}

static void cmd_quit(int argc, char **argv) {
   if (argc != 1) {
      print_usage(argv[0]);
      return;
   }

   g_quit_requested = true;
}

static void cmd_realloc(int argc, char **argv) {
   NarfByteSize size;
   bool result;

   if (argc != 3 || !parse_size_arg(argv[2], &size)) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_realloc_key(%s,%lu)=%s\n",
         argv[1], (unsigned long) size,
         tf[result ASSIGN narf_realloc_key(argv[1], size)]);
}

static void cmd_rename(int argc, char **argv) {
   bool result;

   if (argc != 3) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_rename(%s,%s)=%d\n",
         argv[1], argv[2], result ASSIGN narf_rename_key(argv[1], argv[2]));
}

static void cmd_scan(int argc, char **argv) {
   char *result;

   if (argc != 2) {
      print_usage(argv[0]);
      return;
   }

   result = (char *)narf_metadata(argv[1]);
   printf("narf_metadata(%s)=%s\n",
         argv[1], result ? result : "(null)");
}

static void cmd_slurp(int argc, char **argv) {
   char p[512];
   FILE *f;
   bool result;

   if (argc != 2) {
      print_usage(argv[0]);
      return;
   }

   f = fopen(argv[1], "r");

   if (f) {
      while (fgets(p, sizeof(p), f)) {
         size_t len = strlen(p);

         while (len > 0 && (unsigned char) p[len - 1] < ' ') {
            p[--len] = 0;
         }

         if (len == 0) {
            continue;
         }

         printf("narf_alloc(%s,%d)=%s\n",
               p, 1024, tf[result ASSIGN narf_alloc(p, 1024)]);
      }
      fclose(f);
   }
}

static void cmd_tag(int argc, char **argv) {
   char data[NARF_METADATA_SIZE] = { 0 };
   bool result;

   if (argc < 3 || !join_args(argc, argv, 2, data, sizeof(data))) {
      print_usage(argv[0]);
      return;
   }

   printf("narf_set_metadata(%s,%s)=%s\n",
         argv[1], data, tf[result ASSIGN narf_set_metadata(argv[1], (uint8_t *)data)]);
}

static void cmd_touch(int argc, char **argv) {
   bool result;

   if (argc < 2) {
      print_usage(argv[0]);
      return;
   }

   result = narf_alloc(argv[1], 0);

   printf("touch(%s)=%s\n",
         argv[1], tf[result]);
}

//! @brief Parse and execute one tester command line.
void process_cmd(const char *buffer) {
   char line[1024];
   char *argv[TESTER_MAX_ARGS];
   int argc;
   int n;
   const TesterCommand *cmd;

   if (buffer == NULL) {
      buffer = "";
   }

   n = snprintf(line, sizeof(line), "%s", buffer);
   if (n < 0 || (size_t)n >= sizeof(line)) {
      printf("huh? Command line too long.\n");
      return;
   }

   argc = split_args(line, argv, TESTER_MAX_ARGS);
   if (argc == 0) {
      printf("huh? Empty command.\n");
      print_help();
      return;
   }
   if (argc == -1) {
      printf("huh? Too many arguments.\n");
      return;
   }
   if (argc == -2) {
      printf("huh? Unterminated quote.\n");
      return;
   }

   cmd = find_command(argv[0]);
   if (cmd == NULL) {
      printf("huh? Unknown command '%s'.\n", argv[0]);
      print_help();
      return;
   }

   cmd->fn(argc, argv);
}

char *rname(int l) {
   static char buf[16];
   char *p;
   for (p = buf; p != buf + l; p++) {
      *p = 0x61 + lrand48() % 26;
   }
   *p = 0;
   return buf;
}

//! @brief Run randomized tester operations for stress testing.
void gremlins(int s, int n) {
   char buf[1024];
   int m;
   int l;

   printf("gremlins %d %d\n", s, n);

   srand48(s);

   l = lrand48() % 7 + 1; // length of keys

   process_cmd("mbr");
   process_cmd("partition 1");
   process_cmd("format 1");
   process_cmd("mount 1");

   for(m = 0; m < n; m++) {
      switch(lrand48() % 6) {
         case 0:
            sprintf(buf, "alloc %s %d", rname(l), (int)(lrand48() % 65536));
            break;
         case 1:
            sprintf(buf, "free %s", rname(l));
            break;
         case 2:
            sprintf(buf, "realloc %s %d", rname(l), (int)(lrand48() % 65536));
            break;
         case 3:
            {
               char *p = rname(l);
               sprintf(buf, "tag %s md_%s_md_%04x", p, p, (int)(lrand48() % 65536));
            }
            break;
         case 4:
            {
               int z = lrand48() % 100;
               switch(z) {
                  case 0:
                     sprintf(buf, "defrag");
                     break;
                  default:
                     sprintf(buf, "cat %s", rname(l));
               }
            }
            break;
         case 5:
            {
               char *n1 = strdup(rname(l));
               char *n2 = strdup(rname(l));
               sprintf(buf, "rename %s %s", n1, n2);
               free(n1);
               free(n2);
            }
            break;
      }
      printf("\n");
      printf("###################\n");
      printf("#### GREMLINS %d %d: %s\n", s, m, buf);
      printf("###################\n");
      printf("\n");
      process_cmd(buf);
      printf("\nAFTER:\n");
      narf_debug();
      printf("\n");
   }

   //narf_debug();
   //narf_io_close();
   //exit(0);
}

//! @brief Return true when a line contains at least one non-space byte.
static bool line_has_text(const char *line) {
   if (line == NULL) {
      return false;
   }

   while (*line != 0) {
      if ((unsigned char)*line > ' ') {
         return true;
      }
      ++line;
   }

   return false;
}

//! @brief Run the interactive tester prompt.
void loop(void) {
   g_quit_requested = false;

   if (isatty(STDIN_FILENO)) {
      char *line;

      while (!g_quit_requested && (line = readline("#>")) != NULL) {
         if (line_has_text(line)) {
            add_history(line);
         }

         process_cmd(line);
         free(line);
      }
   }
   else {
      char buffer[1024];

      printf("#>");
      while(!g_quit_requested && fgets(buffer, sizeof(buffer), stdin)) {
         size_t len = strlen(buffer);

         while (len > 0 && (unsigned char) buffer[len - 1] < ' ') {
            buffer[--len] = 0;
         }

         process_cmd(buffer);

         if (!g_quit_requested) {
            printf("#>");
         }
      }
   }
}

//! @brief Open the image and run the tester prompt.
int main(int argc, char **argv) {
   bool result;

   printf("NARF example\n");

   if (argc != 2) {
      fprintf(stderr, "Usage: %s <filename|=size,filename>\n", argv[0]);
      exit(0);
   }
   narf_io_configure(argv[1]);

   printf("narf_io_open()=%d\n", result ASSIGN narf_io_open());

   if (result) {
      printf("narf_io_sectors()=%08X\n", narf_io_sectors());

      loop();

      printf("narf_io_close()=%d\n", result ASSIGN narf_io_close());
   }
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
