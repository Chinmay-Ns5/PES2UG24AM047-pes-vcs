// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} FlatEntry;

static int flat_entry_cmp(const void *a, const void *b) {
    return strcmp(((const FlatEntry *)a)->path, ((const FlatEntry *)b)->path);
}

static int build_tree(FlatEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    size_t prefix_len = strlen(prefix);

    int i = 0;
    while (i < count) {
        const char *rel_path = (prefix_len == 0)
            ? entries[i].path
            : entries[i].path + prefix_len + 1;

        const char *slash = strchr(rel_path, '/');

        if (!slash) {
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = entries[i].mode;
            e->hash = entries[i].hash;
            strncpy(e->name, rel_path, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            i++;
        } else {
            int dir_len = (int)(slash - rel_path);
            if (dir_len >= (int)sizeof(((TreeEntry *)0)->name)) return -1;

            char dir_name[256];
            strncpy(dir_name, rel_path, (size_t)dir_len);
            dir_name[dir_len] = '\0';

            char new_prefix[1024];
            if (prefix_len == 0)
                snprintf(new_prefix, sizeof(new_prefix), "%s", dir_name);
            else
                snprintf(new_prefix, sizeof(new_prefix), "%s/%s", prefix, dir_name);

            int j = i;
            while (j < count) {
                const char *p = (prefix_len == 0)
                    ? entries[j].path
                    : entries[j].path + prefix_len + 1;
                if (strncmp(p, dir_name, (size_t)dir_len) == 0 && p[dir_len] == '/')
                    j++;
                else
                    break;
            }

            ObjectID subtree_id;
            if (build_tree(entries + i, j - i, new_prefix, &subtree_id) != 0)
                return -1;

            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = 0040000;
            e->hash = subtree_id;
            strncpy(e->name, dir_name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';

            i = j;
        }
    }

    void *data; size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    FILE *f = fopen(INDEX_FILE, "r");

    FlatEntry *entries = malloc(10000 * sizeof(FlatEntry));
    if (!entries) { if (f) fclose(f); return -1; }
    int count = 0;

    if (f) {
        while (count < 10000) {
            FlatEntry *e = &entries[count];
            char hex[HASH_HEX_SIZE + 1];
            unsigned int mode;
            unsigned long long mtime;
            unsigned int size;
            int rc = fscanf(f, "%o %64s %llu %u %511s",
                            &mode, hex, &mtime, &size, e->path);
            if (rc == EOF || rc != 5) break;
            e->mode = (uint32_t)mode;
            if (hex_to_hash(hex, &e->hash) != 0) {
                free(entries); fclose(f); return -1;
            }
            count++;
        }
        fclose(f);
    }

    qsort(entries, count, sizeof(FlatEntry), flat_entry_cmp);
    int rc = build_tree(entries, count, "", id_out);
    free(entries);
    return rc;
}
