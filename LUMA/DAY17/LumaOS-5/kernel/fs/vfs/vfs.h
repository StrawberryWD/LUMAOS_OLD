/* kernel/fs/vfs/vfs.h
 * LumaOS 5.0 - Virtual File System Abstraction
 *
 * DAY8 (Week 2, Day 8 of the 5.0 filesystem roadmap):
 *   "创建 kernel/fs/vfs/vfs.h。定义 struct file、struct vfs_node、
 *    struct filesystem。定义函数指针：open、read、readdir。"
 *
 *   This header is the filesystem-independent layer that every concrete
 *   filesystem driver (FAT12 starting Day 9-14, FAT16/32 in Week 3, the
 *   custom LMFS in Month 2) is expected to plug into — mirroring how
 *   kernel/drivers/block/block.h (Day 6) decoupled upper-layer code
 *   from *which* disk controller is underneath. From here on, Shell
 *   commands and higher-level code (`ls`, `cat`, ...) are meant to walk
 *   `struct vfs_node` trees and call through a `struct filesystem`'s
 *   function pointers — never a driver-specific function
 *   (`fat12_readdir()`, `lmfs_read()`, etc.) by name.
 *
 *   Day 8's acceptance criteria is intentionally narrow: "头文件编译
 *   通过" (the header compiles). No .c file, no VFS state machine, and
 *   no block_device_t wiring is implemented yet — that starts Day 9
 *   (FAT12 BPB parsing) and Day 13 ("将 FAT12 挂接到 VFS: 注册
 *   fat12_filesystem_ops. 实现 vfs_mount("/dev/hda")"), which is where
 *   vfs_mount()/vfs_readdir() gain bodies and kmain.c/shell.c start
 *   calling into this layer. This file only needs to declare the
 *   shapes those later steps will fill in, so today it's included
 *   nowhere yet (deliberately not added to build.sh's CFLAGS -I list
 *   or any #include — there is no .c to compile against it until
 *   Day 9+). Standalone compilation (`gcc -fsyntax-only vfs.h`-style,
 *   wrapped in a throwaway .c) is how the Day 8 acceptance check is
 *   verified.
 *
 * Design notes (kept consistent with block.h's conventions):
 *   - Every function pointer takes the owning struct as its first
 *     argument (mirrors block_read_fn's `block_device_t *dev` pattern)
 *     so a filesystem backend can support multiple mounted instances
 *     without file-scope globals, and so a node's operations can find
 *     their way back to the node without a second lookup.
 *   - `open`/`read`/`readdir` all return int (0 success / negative
 *     error), matching the existing ide_ and block_ function naming
 *     convention of 0-on-success so callers migrating between layers
 *     don't need to change their error handling style. Week 10
 *     (Day 70) is where the roadmap formalizes named error codes
 *     (-ENOENT, -EIO); until then -1 is the generic "failed" value,
 *     same as mbr_parse()'s contract.
 *   - `struct vfs_node` is the in-memory, filesystem-independent
 *     handle for anything in the tree (file OR directory — VFS_TYPE_*
 *     distinguishes them), analogous to a Unix vnode. It carries a
 *     `void *fs_private` for the backend to stash its own on-disk
 *     handle (e.g. a FAT12 starting cluster + parent directory sector,
 *     or an LMFS inode number) without polluting this generic struct —
 *     the same pattern block_device_t's `dev_private` uses for driver
 *     state.
 *   - `struct file` is the per-open-instance handle (an open vfs_node
 *     plus a byte offset). Day 20 ("增加文件偏移指针 file->pos，支持
 *     多次读取") is what actually exercises `pos` across repeated
 *     reads; it's declared now so `read`'s signature is stable across
 *     that later step instead of changing later.
 *   - `struct filesystem` bundles a filesystem driver's operations
 *     table (what block_device_t did for disk drivers) plus a `name`
 *     tag and a `root` node once mounted. Day 13's vfs_mount() is what
 *     will allocate/populate one of these for FAT12; this header just
 *     defines its shape.
 *   - No block_device_t include here on purpose: Day 8's scope is the
 *     VFS's own node/file/filesystem shapes only. The concrete Day 9+
 *     FAT12 backend is what will pull in block.h to talk to disk.
 */
#ifndef LUMA_VFS_H
#define LUMA_VFS_H

#include <stdint.h>
#include <stddef.h>

/* ===== Node types =====
 * What kind of thing a struct vfs_node represents. Kept as a small
 * bitless enum (not flags) since Day 8's tree is strictly "file" or
 * "directory" — same two-way split FAT's directory-entry attribute
 * byte and LMFS's planned inode "type" field (Day 37: "lmfs_create_
 * inode(type)") both use. */
typedef enum {
    VFS_TYPE_FILE = 0,
    VFS_TYPE_DIR  = 1,
} vfs_node_type_t;

/* ===== Forward declarations =====
 * struct filesystem, struct vfs_node, and struct file all reference
 * each other (a node points back at its owning filesystem, a file
 * wraps a node, a filesystem's ops take a node), so every struct name
 * must be visible before all three are fully defined. */
struct filesystem;
struct vfs_node;
struct file;

/* ===== VFS-level function pointer types =====
 *
 * vfs_open_fn(node, out_file)
 *   Prepares `*out_file` to read from `node` (resets file->pos to 0,
 *   stashes the node pointer, lets the backend do any per-open setup
 *   it needs). Returns 0 on success, negative on error (e.g. node is
 *   a directory and the caller asked to open it as a file).
 *
 * vfs_read_fn(file, buf, size)
 *   Reads up to `size` bytes from `file` (starting at file->pos) into
 *   `buf`, advancing file->pos by the number of bytes actually read.
 *   Returns the number of bytes read (>= 0; 0 at end-of-file), or a
 *   negative value on error — this differs from block_read_fn's
 *   strict 0/-1 because callers need to know a short-read count, the
 *   same way a Unix read(2) does.
 *
 * vfs_readdir_fn(dir_node, index, out_node)
 *   Fetches the `index`-th child (0-based) of `dir_node` (which must
 *   be VFS_TYPE_DIR) into `*out_node`. Returns 0 on success, -1 if
 *   `index` is past the last entry (the caller's signal to stop
 *   iterating) or dir_node is not a directory.
 */
typedef int (*vfs_open_fn)(struct vfs_node *node, struct file *out_file);
typedef int (*vfs_read_fn)(struct file *file, void *buf, uint32_t size);
typedef int (*vfs_readdir_fn)(struct vfs_node *dir_node, uint32_t index,
                               struct vfs_node *out_node);

/* ===== struct vfs_node =====
 *
 * The filesystem-independent, in-memory handle for one entry (file or
 * directory) somewhere in a mounted filesystem's tree — the VFS
 * equivalent of block_device_t for disk drivers. Upper-layer code
 * (Shell commands, path-walking helpers added Day 18+) is meant to
 * hold and pass around `struct vfs_node *` rather than any backend-
 * specific handle.
 */
typedef struct vfs_node {
    char                name[32];   /* entry name, NUL-terminated, WITHOUT a
                                        trailing '/' even for directories
                                        (e.g. "IDE0", not "IDE0/") —
                                        LumaOS's own namespace diagram
                                        (计划.md) writes every directory
                                        with a trailing '/' (IDE/,
                                        System/, ...), but that is a
                                        DISPLAY convention only, applied
                                        by whoever prints a node (check
                                        `type == VFS_TYPE_DIR` and
                                        append '/'). Keeping the stored
                                        name slash-free avoids double-
                                        slash bugs once Day 18 starts
                                        concatenating path segments (e.g.
                                        "IDE0" + "/" + "System"); 8.3-
                                        style names fit comfortably
                                        either way, long-name support if
                                        any is later */
    vfs_node_type_t     type;       /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    uint32_t            size;       /* file size in bytes; 0 for directories */
    struct filesystem  *fs;         /* owning filesystem (for fs->ops calls) */
    void               *fs_private; /* backend-specific handle, opaque here
                                        (e.g. FAT12 starting cluster, or an
                                        LMFS inode number) */
} vfs_node_t;

/* ===== struct file =====
 *
 * A single open instance of a vfs_node, with its own read offset.
 * Multiple `struct file`s may reference the same vfs_node concurrently
 * (each with an independent `pos`), matching how a Unix file
 * descriptor's offset is per-open, not per-inode.
 */
typedef struct file {
    vfs_node_t *node;  /* the node this file was opened from */
    uint32_t    pos;   /* current byte offset for the next read */
} file_t;

/* ===== struct filesystem =====
 *
 * A mounted filesystem instance: its driver's operations table plus
 * the root node reachable through it. Day 13 ("将 FAT12 挂接到 VFS:
 * 注册 fat12_filesystem_ops. 实现 vfs_mount(\"IDE\")") is what
 * will populate `ops` with FAT12's function pointers and set `root`
 * to the parsed root directory; this header only defines the shape.
 */
typedef struct filesystem {
    const char       *name;     /* e.g. "fat12", "fat16", "fat32", "lmfs" */
    vfs_open_fn       open;     /* required: fs->open(node, out_file) */
    vfs_read_fn       read;     /* required: fs->read(file, buf, size) */
    vfs_readdir_fn    readdir;  /* required for dirs: fs->readdir(dir, i, out) */
    vfs_node_t       *root;     /* root directory node once mounted, else NULL */
    void             *fs_private; /* backend-specific mount state, opaque here
                                      (e.g. a parsed BPB, or an LMFS superblock) */
} filesystem_t;

/* ===== DAY13 (Week 2, Day 13 of the 5.0 filesystem roadmap) =====
 *
 * "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
 *  vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
 *  根目录。"
 *
 *   This is where the shapes this header has declared since Day 8
 *   finally get a mount entry point. The actual FAT12-specific glue
 *   (fat12_filesystem_ops, the adapter functions) lives in
 *   kernel/fs/fat/fat12_vfs.{h,c} — deliberately NOT included here,
 *   the same way this header has never included fat12.h: vfs.h stays
 *   filesystem-agnostic, vfs.c (kernel/fs/vfs/vfs.c) is what actually
 *   knows FAT12 exists and calls into fat12_vfs_mount().
 *
 * vfs_root
 *   The single global "current root of the mounted filesystem"
 *   pointer the roadmap's acceptance text names directly. NULL until
 *   a successful vfs_mount() call. This project has no multi-mount
 *   namespace yet (a real mount TABLE, letting several filesystems
 *   coexist under different paths, is Day 35's "FAT 与 LMFS 可以同时
 *   存在" and beyond) — through Day 13/14, "the" root is simply
 *   whatever the one supported device string ("IDE0") last mounted.
 *
 * vfs_mount(path)
 *   Mounts a filesystem and points vfs_root at its root directory.
 *   `path` is a DEVICE name, not a filesystem path to mount onto.
 *   Per LumaOS's own namespace document (计划.md's file-tree
 *   diagram), this project does NOT use Unix-style "/dev/hda" device
 *   paths — the top level of the physical-disk namespace is `IDE/`
 *   itself (a peer of `USB/`, `IMG/`, `ISO/`, with `Boot/`, `System/`,
 *   `User/`, `Administrator/`, `Public/` beneath it). The device
 *   string itself is further NUMBERED — "IDE0", "IDE1", "IDE2", ... —
 *   one per physical IDE drive in probe order, mirroring how
 *   block.c's accessor is `ide0_block_device()` (not a bare
 *   `ide_block_device()`) and this project's own `USB/DISK_1`,
 *   `USB/DISK_2` convention already numbers multiple like-devices for
 *   the same reason: a single unnumbered "IDE" can't name a second
 *   drive once one exists. So `"IDE0"` is the device string
 *   vfs_mount() recognizes — today the ONLY one it recognizes, since
 *   block.c only implements ide0_block_device() so far (see
 *   kernel/fs/vfs/vfs.c's header comment for the full rationale and
 *   for exactly how an "IDE1" branch gets added once a second drive
 *   is real). A real path/mount-point namespace resolver (turning
 *   e.g. "IDE0/System/Kernel" into a walked vfs_node_t chain) is
 *   later work (Day 18's fat_open()-by-path is the first piece of
 *   that, Day 68-69's "df"/"cd"/"pwd" the rest) — kernel/fs/vfs/vfs.c
 *   documents exactly which physical LBA "IDE0" resolves to on THIS
 *   project's test image, since there's no real FAT-partitioned boot
 *   disk yet (see kmain.c's DAY5/DAY9 comments on why LBA 0 here is
 *   LumaOS's own mbr.asm, not a FAT partition table with real
 *   entries).
 *
 *   Returns 0 on success (vfs_root now points at the mounted
 *   filesystem's root VFS_TYPE_DIR node). Returns -1 if `path` isn't
 *   recognized, or if the underlying filesystem-specific mount call
 *   fails (vfs_root is left unchanged on failure — a failed
 *   vfs_mount() call must never clobber a previously-good mount).
 */
extern vfs_node_t *vfs_root;

int vfs_mount(const char *path);

#endif /* LUMA_VFS_H */