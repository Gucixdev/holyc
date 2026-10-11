#ifndef HOLYC_FILEDIALOGUE_H_
#define HOLYC_FILEDIALOGUE_H_

/* TUI file picker - a directory listing + cursor + navigation state
 * machine, rendered onto tui.h's cell buffer. Split the same way tui.h
 * itself is split: the navigation LOGIC (list/move/descend/ascend/select)
 * is pure state manipulation over already-read directory entries, fully
 * testable without a real terminal - only file_dialogue_scan (the one
 * function that actually calls opendir/readdir) touches the real
 * filesystem, and even that needs no tty, just a real directory (e.g. a
 * throwaway mkdtemp() tree in a test). The untestable-without-a-real-
 * terminal boundary tui.h itself draws is entirely in
 * tui_raw_mode_enable/tui_present's caller, not here.
 *
 * Entries are capped at FILE_DIALOGUE_MAX_ENTRIES per directory (arena-
 * allocated up to that cap, not silently unbounded) - same "a mesh
 * importer isn't a general CAD tool" reasoning obj.h gives for its own
 * OBJ_MAX_FACE_VERTS cap: a file picker is for picking a file a human
 * can see on one screen, not indexing an arbitrarily large directory. */

#include "base.h"
#include "arena_alloc.h"
#include "tui.h"

#ifndef FILE_DIALOGUE_MAX_ENTRIES
#define FILE_DIALOGUE_MAX_ENTRIES 4096
#endif

#ifndef FILE_DIALOGUE_NAME_CAP
#define FILE_DIALOGUE_NAME_CAP 256
#endif

#ifndef FILE_DIALOGUE_PATH_CAP
#define FILE_DIALOGUE_PATH_CAP 4096
#endif

typedef struct {
    char name[FILE_DIALOGUE_NAME_CAP];
    i32 is_dir;
} File_Dialogue_Entry;

typedef struct {
    Arena *a;
    char cwd[FILE_DIALOGUE_PATH_CAP];
    File_Dialogue_Entry *entries;  /* arena-owned, up to FILE_DIALOGUE_MAX_ENTRIES */
    size_t entry_count;
    size_t cursor;                 /* index into entries, 0 if entry_count == 0 */
} File_Dialogue;

typedef enum {
    FILE_DIALOGUE_ESUCCESS   = 0,
    FILE_DIALOGUE_ENOTFOUND  = -1, /* opendir failed (missing/no permission) */
    FILE_DIALOGUE_ETOOLONG   = -2  /* a resulting path would exceed FILE_DIALOGUE_PATH_CAP */
} File_Dialogue_Error;

/* opens `path` and populates entries (sorted: directories first, then
 * alphabetically within each group - the conventional file-picker
 * ordering). Entries beyond FILE_DIALOGUE_MAX_ENTRIES are silently
 * dropped (documented cap, see file header), not an error. */
i32 file_dialogue_init(File_Dialogue *fd, Arena *a, const char *path);

/* re-scans the current cwd (e.g. after external changes) - same
 * population logic init uses, cursor reset to 0. */
i32 file_dialogue_rescan(File_Dialogue *fd);

/* moves the cursor by `delta` (negative = up), clamped to
 * [0, entry_count-1]. A no-op on an empty directory. */
void file_dialogue_move_cursor(File_Dialogue *fd, i32 delta);

/* if the entry under the cursor is a directory, descends into it
 * (rescans) and returns 1 with out_path untouched. If it's a file,
 * writes the full path into out_path (>= FILE_DIALOGUE_PATH_CAP bytes)
 * and returns 2 (a file was SELECTED, not just navigated). Returns 0 on
 * an empty directory (nothing under the cursor) or FILE_DIALOGUE_ETOOLONG
 * if the resulting path wouldn't fit. */
i32 file_dialogue_enter(File_Dialogue *fd, char *out_path);

/* navigates to the parent directory (rescans). A no-op (returns 0) at
 * the filesystem root; returns 1 otherwise. */
i32 file_dialogue_up(File_Dialogue *fd);

/* renders the current entry list into `buf` starting at (x, y), one
 * entry per row, up to `max_rows` rows - the entry under the cursor is
 * drawn with fg/bg swapped from the rest so it reads as highlighted. */
void file_dialogue_render(const File_Dialogue *fd, Tui_Buffer *buf, i32 x, i32 y, i32 max_rows,
                           u16 fg, u16 bg);

const char *file_dialogue_strerror(i32 err);

#endif /* HOLYC_FILEDIALOGUE_H_ */

#ifdef FILE_DIALOGUE_IMPLEMENTATION
#ifndef FILE_DIALOGUE_IMPLEMENTATION_
#define FILE_DIALOGUE_IMPLEMENTATION_

#include <dirent.h>
#include <sys/stat.h>

const char *file_dialogue_strerror(i32 err)
{
    switch (err) {
    case FILE_DIALOGUE_ESUCCESS:  return "success";
    case FILE_DIALOGUE_ENOTFOUND: return "cannot open directory";
    case FILE_DIALOGUE_ETOOLONG:  return "resulting path too long";
    default: break;
    }
    return "unknown error";
}

static i32 file_dialogue_entry_less_(const File_Dialogue_Entry *a, const File_Dialogue_Entry *b)
{
    if (a->is_dir != b->is_dir) return a->is_dir > b->is_dir; /* directories first */
    return strcmp(a->name, b->name) < 0;
}

/* insertion sort - entry counts here are screen-sized (capped at
 * FILE_DIALOGUE_MAX_ENTRIES, realistically far fewer), not a hot path
 * worth a real sort algorithm over. */
static void file_dialogue_sort_(File_Dialogue_Entry *items, size_t count)
{
    size_t i, j;
    for (i = 1; i < count; i++) {
        File_Dialogue_Entry key = items[i];
        j = i;
        while (j > 0 && file_dialogue_entry_less_(&key, &items[j - 1])) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = key;
    }
}

static i32 file_dialogue_join_(char *out, const char *dir, const char *name)
{
    size_t dn = arena_strlen(dir);
    size_t nn = arena_strlen(name);
    i32 need_sep = (dn > 0 && dir[dn - 1] != '/');
    size_t total = dn + (size_t)need_sep + nn;

    if (total >= FILE_DIALOGUE_PATH_CAP) return 0;

    arena_memcpy(out, dir, dn);
    if (need_sep) out[dn] = '/';
    arena_memcpy(out + dn + (size_t)need_sep, name, nn);
    out[total] = 0;
    return 1;
}

static i32 file_dialogue_scan_(File_Dialogue *fd)
{
    DIR *dir;
    struct dirent *ent;
    File_Dialogue_Entry *entries;
    size_t count = 0;

    dir = opendir(fd->cwd);
    if (!dir) return FILE_DIALOGUE_ENOTFOUND;

    entries = (File_Dialogue_Entry *)arena_alloc(fd->a, FILE_DIALOGUE_MAX_ENTRIES * sizeof(File_Dialogue_Entry));

    while ((ent = readdir(dir)) != 0) {
        size_t n;

        if (strcmp(ent->d_name, ".") == 0) continue;
        if (count >= FILE_DIALOGUE_MAX_ENTRIES) break; /* documented cap - see file header */

        n = arena_strlen(ent->d_name);
        if (n >= FILE_DIALOGUE_NAME_CAP) n = FILE_DIALOGUE_NAME_CAP - 1;
        arena_memcpy(entries[count].name, ent->d_name, n);
        entries[count].name[n] = 0;

        /* deliberately stat()-based, not readdir's own d_type/DT_DIR -
         * DT_DIR is real but sits behind glibc's __USE_MISC, which needs
         * _DEFAULT_SOURCE defined before the FIRST system header this
         * translation unit ever includes (a whole-TU ordering
         * requirement a single header like this one can't reliably
         * enforce on whatever includes it first) - found the hard way,
         * DT_DIR silently compiled to 0/undefined under this project's
         * own -D_POSIX_C_SOURCE=200112L and is_dir was always false.
         * stat() is plain POSIX.1-2001, no feature-macro dependency. */
        {
            char full_path[FILE_DIALOGUE_PATH_CAP];
            struct stat st;
            entries[count].is_dir = 0;
            if (file_dialogue_join_(full_path, fd->cwd, ent->d_name) &&
                stat(full_path, &st) == 0) {
                entries[count].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            }
        }
        count++;
    }
    closedir(dir);

    file_dialogue_sort_(entries, count);

    fd->entries = entries;
    fd->entry_count = count;
    fd->cursor = 0;
    return FILE_DIALOGUE_ESUCCESS;
}

i32 file_dialogue_init(File_Dialogue *fd, Arena *a, const char *path)
{
    size_t n = arena_strlen(path);
    if (n >= sizeof(fd->cwd)) return FILE_DIALOGUE_ETOOLONG;

    memset(fd, 0, sizeof(*fd));
    fd->a = a;
    arena_memcpy(fd->cwd, path, n);
    fd->cwd[n] = 0;

    return file_dialogue_scan_(fd);
}

i32 file_dialogue_rescan(File_Dialogue *fd)
{
    return file_dialogue_scan_(fd);
}

void file_dialogue_move_cursor(File_Dialogue *fd, i32 delta)
{
    i64 next;

    if (fd->entry_count == 0) return;

    next = (i64)fd->cursor + delta;
    if (next < 0) next = 0;
    if (next >= (i64)fd->entry_count) next = (i64)fd->entry_count - 1;
    fd->cursor = (size_t)next;
}

i32 file_dialogue_enter(File_Dialogue *fd, char *out_path)
{
    char joined[FILE_DIALOGUE_PATH_CAP];
    const File_Dialogue_Entry *ent;

    if (fd->entry_count == 0) return 0;
    ent = &fd->entries[fd->cursor];

    if (strcmp(ent->name, "..") == 0) return file_dialogue_up(fd);

    if (!file_dialogue_join_(joined, fd->cwd, ent->name)) return FILE_DIALOGUE_ETOOLONG;

    if (ent->is_dir) {
        size_t n = arena_strlen(joined);
        if (n >= sizeof(fd->cwd)) return FILE_DIALOGUE_ETOOLONG;
        arena_memcpy(fd->cwd, joined, n);
        fd->cwd[n] = 0;
        return file_dialogue_scan_(fd) == FILE_DIALOGUE_ESUCCESS ? 1 : FILE_DIALOGUE_ENOTFOUND;
    }

    {
        size_t n = arena_strlen(joined);
        arena_memcpy(out_path, joined, n);
        out_path[n] = 0;
    }
    return 2;
}

i32 file_dialogue_up(File_Dialogue *fd)
{
    size_t n = arena_strlen(fd->cwd);
    size_t i;

    if (n == 0 || (n == 1 && fd->cwd[0] == '/')) return 0; /* already at root */

    i = n;
    while (i > 0 && fd->cwd[i - 1] != '/') i--;
    while (i > 1 && fd->cwd[i - 1] == '/') i--; /* drop the trailing slash too, unless it's root's own */

    if (i == 0) {
        fd->cwd[0] = '/';
        fd->cwd[1] = 0;
    } else {
        fd->cwd[i] = 0;
    }

    return file_dialogue_scan_(fd) == FILE_DIALOGUE_ESUCCESS ? 1 : FILE_DIALOGUE_ENOTFOUND;
}

void file_dialogue_render(const File_Dialogue *fd, Tui_Buffer *buf, i32 x, i32 y, i32 max_rows,
                           u16 fg, u16 bg)
{
    i32 row;
    size_t i;

    for (row = 0, i = 0; row < max_rows && i < fd->entry_count; row++, i++) {
        const char *name = fd->entries[i].name;
        i32 col = x;
        u16 cell_fg = fg, cell_bg = bg;

        if (i == fd->cursor) {
            cell_fg = bg;
            cell_bg = fg;
        }

        while (*name) {
            u32 cp;
            i32 adv = tui_utf8_char_to_unicode(&cp, name);
            if (adv < 0) break;
            tui_buffer_set_cell(buf, col, y + row, cp, cell_fg, cell_bg);
            name += adv;
            col++;
        }
    }
}

#endif /* FILE_DIALOGUE_IMPLEMENTATION_ */
#endif /* FILE_DIALOGUE_IMPLEMENTATION */
