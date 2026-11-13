/* fat12.c - FAT12 文件系统实现（含子目录） */
#include "fat12.h"
#include "ata.h"
#include "lib.h"
#include "kernel.h"

struct bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
} __attribute__((packed));

static fat12_layout_t  layout;
static int             mounted = 0;
static uint8_t         fat_buf[9 * 512];
static uint8_t         sec_buf[512];

/* 当前目录 */
static uint16_t cwd_cluster = 0;              /* 0 = 根目录 */
static char     cwd_path[256] = "";           /* "" = 根目录 */

static int read_sector(uint32_t lba, uint8_t* buf) {
    return ata_read_sector(lba, buf);
}
static int write_sector(uint32_t lba, const uint8_t* buf) {
    return ata_write_sector(lba, buf);
}

/* ---------- 名字处理 ---------- */

static void format_83(const uint8_t* raw, char* out) {
    int pos = 0;
    for (int i = 0; i < 8; i++) {
        if (raw[i] == ' ') break;
        out[pos++] = (char)raw[i];
    }
    if (raw[8] != ' ') {
        out[pos++] = '.';
        for (int i = 8; i < 11; i++) {
            if (raw[i] == ' ') break;
            out[pos++] = (char)raw[i];
        }
    }
    out[pos] = 0;
}

static int parse_83(const char* src, uint8_t* out) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (!src || !*src) return -1;

    /* ★ 特判 . 和 .. */
    if (src[0] == '.' && src[1] == 0)    { out[0] = '.'; return 0; }
    if (src[0] == '.' && src[1] == '.' && src[2] == 0) {
        out[0] = '.'; out[1] = '.'; return 0;
    }

    int i = 0, o = 0;
    while (*src && *src != '.' && o < 8) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[o++] = (uint8_t)c;
        i++;
    }
    if (i == 0) return -1;
    if (*src == '.') {
        src++;
        o = 8;
        while (*src && o < 11) {
            char c = *src++;
            if (c >= 'a' && c <= 'z') c -= 32;
            out[o++] = (uint8_t)c;
        }
    }
    return 0;
}

/* ---------- FAT 表项 ---------- */

static uint16_t fat12_get(uint32_t cluster) {
    uint32_t off = cluster + (cluster / 2);
    if (off + 1 >= layout.fat_sectors * 512) return 0xFFF;
    uint16_t raw = fat_buf[off] | ((uint16_t)fat_buf[off + 1] << 8);
    return (cluster & 1) ? (uint16_t)(raw >> 4) : (uint16_t)(raw & 0x0FFF);
}

static int fat12_set(uint32_t cluster, uint16_t value) {
    uint32_t off = cluster + (cluster / 2);
    if (off + 1 >= layout.fat_sectors * 512) return FAT12_ERR_FULL;
    uint16_t raw = fat_buf[off] | ((uint16_t)fat_buf[off + 1] << 8);
    value &= 0x0FFF;
    if (cluster & 1) raw = (raw & 0x000F) | (value << 4);
    else             raw = (raw & 0xF000) | value;
    fat_buf[off]     = (uint8_t)(raw & 0xFF);
    fat_buf[off + 1] = (uint8_t)(raw >> 8);
    return FAT12_OK;
}

static int fat12_flush_fat(void) {
    for (uint32_t i = 0; i < layout.fat_sectors; i++)
        if (write_sector(layout.fat_start + i, fat_buf + i * 512) != ATA_OK)
            return FAT12_ERR_IO;
    if (layout.num_fats >= 2)
        for (uint32_t i = 0; i < layout.fat_sectors; i++)
            if (write_sector(layout.fat_start + layout.fat_sectors + i,
                             fat_buf + i * 512) != ATA_OK)
                return FAT12_ERR_IO;
    return FAT12_OK;
}

static uint32_t fat12_alloc_cluster(void) {
    for (uint32_t c = 2; c < layout.cluster_count + 2; c++)
        if (fat12_get(c) == 0) return c;
    return 0;
}

/* ---------- 目录层 ---------- */

/* 读目录的第 sec_idx 个扇区（dir_cluster==0 = 根目录） */
static int dir_read_sec(uint16_t dir_cluster, uint32_t sec_idx, uint8_t* buf) {
    if (dir_cluster == 0) {
        if (sec_idx >= layout.root_sectors) return FAT12_ERR_INVALID;
        return read_sector(layout.root_start + sec_idx, buf) == ATA_OK
               ? FAT12_OK : FAT12_ERR_IO;
    }
    /* 子目录：沿簇链走 */
    uint32_t c   = dir_cluster;
    uint32_t spc = layout.sectors_per_cluster;
    uint32_t skip   = sec_idx / spc;
    uint32_t offset = sec_idx % spc;
    while (skip--) {
        c = fat12_get(c);
        if (c < 2 || c >= 0xFF8) return FAT12_ERR_INVALID;
    }
    uint32_t lba = layout.data_start + (c - 2) * spc + offset;
    return read_sector(lba, buf) == ATA_OK ? FAT12_OK : FAT12_ERR_IO;
}

/* 写目录的第 sec_idx 个扇区 */
static int dir_write_sec(uint16_t dir_cluster, uint32_t sec_idx, const uint8_t* buf) {
    if (dir_cluster == 0) {
        if (sec_idx >= layout.root_sectors) return FAT12_ERR_INVALID;
        return write_sector(layout.root_start + sec_idx, buf) == ATA_OK
               ? FAT12_OK : FAT12_ERR_IO;
    }
    uint32_t c   = dir_cluster;
    uint32_t spc = layout.sectors_per_cluster;
    uint32_t skip   = sec_idx / spc;
    uint32_t offset = sec_idx % spc;
    while (skip--) {
        c = fat12_get(c);
        if (c < 2 || c >= 0xFF8) return FAT12_ERR_INVALID;
    }
    uint32_t lba = layout.data_start + (c - 2) * spc + offset;
    return write_sector(lba, buf) == ATA_OK ? FAT12_OK : FAT12_ERR_IO;
}

/* 在目录里找项 */
static int dir_find(uint16_t dir_cluster, const uint8_t* name11,
                    uint8_t* out_entry, uint32_t* out_lba, int* out_idx) {
    uint32_t max_sec = (dir_cluster == 0)
        ? layout.root_sectors
        : layout.sectors_per_cluster;      /* 简化：子目录仅 1 簇 */
    for (uint32_t s = 0; s < max_sec; s++) {
        uint32_t lba = (dir_cluster == 0)
            ? layout.root_start + s
            : layout.data_start + (dir_cluster - 2) * layout.sectors_per_cluster + s;
        if (read_sector(lba, sec_buf) != ATA_OK) return 0;
        for (int i = 0; i < 16; i++) {
            uint8_t* e = sec_buf + i * 32;
            if (e[0] == 0x00) return 0;
            if (e[0] == 0xE5) continue;
            if (e[11] & 0x08) continue;
            if (memcmp(e, name11, 11) == 0) {
                if (out_entry) memcpy(out_entry, e, 32);
                if (out_lba)   *out_lba = lba;
                if (out_idx)   *out_idx = i;
                return 1;
            }
        }
    }
    return 0;
}

/* 在目录里添加项 */
static int dir_add(uint16_t dir_cluster, const uint8_t* name11,
                   uint8_t attr, uint16_t cluster, uint32_t size) {
    uint32_t max_sec = (dir_cluster == 0)
        ? layout.root_sectors
        : layout.sectors_per_cluster;
    for (uint32_t s = 0; s < max_sec; s++) {
        uint32_t lba = (dir_cluster == 0)
            ? layout.root_start + s
            : layout.data_start + (dir_cluster - 2) * layout.sectors_per_cluster + s;
        if (read_sector(lba, sec_buf) != ATA_OK) return FAT12_ERR_IO;
        for (int i = 0; i < 16; i++) {
            uint8_t* e = sec_buf + i * 32;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                memset(e, 0, 32);
                memcpy(e, name11, 11);
                e[11] = attr;
                e[26] = (uint8_t)(cluster & 0xFF);
                e[27] = (uint8_t)(cluster >> 8);
                e[28] = (uint8_t)(size & 0xFF);
                e[29] = (uint8_t)((size >> 8) & 0xFF);
                e[30] = (uint8_t)((size >> 16) & 0xFF);
                e[31] = (uint8_t)((size >> 24) & 0xFF);
                return write_sector(lba, sec_buf) == ATA_OK
                       ? FAT12_OK : FAT12_ERR_IO;
            }
        }
    }
    return FAT12_ERR_FULL;
}

/* 更新目录项 */
static int dir_update(uint32_t lba, int idx, const uint8_t* new_entry) {
    if (read_sector(lba, sec_buf) != ATA_OK) return FAT12_ERR_IO;
    memcpy(sec_buf + idx * 32, new_entry, 32);
    return write_sector(lba, sec_buf) == ATA_OK ? FAT12_OK : FAT12_ERR_IO;
}

/* ---------- 路径解析 ---------- */

/* 返回值：1 = 目标存在（parent,name11,entry 全填），
 *       0 = 父目录存在，目标不存在（parent,name11 填）
 *       负数 = 错误
 * 起点：path[0]=='/' 从根；否则从 CWD */
static int path_resolve(const char* path, uint16_t* out_parent,
                        uint8_t* out_name11, uint8_t* out_entry) {
    uint16_t cur = 0;
    if (*path == '/') {
        cur = 0;
        while (*path == '/') path++;
    } else {
        cur = cwd_cluster;
    }

    if (*path == 0) return FAT12_ERR_INVALID;

    for (;;) {
        char comp[13];
        int n = 0;
        while (*path && *path != '/') {
            if (n < 12) comp[n++] = *path;
            path++;
        }
        comp[n] = 0;
        while (*path == '/') path++;

        uint8_t comp11[11];
        if (parse_83(comp, comp11) != 0) return FAT12_ERR_INVALID;

        uint8_t e[32];
        if (!dir_find(cur, comp11, e, NULL, NULL)) {
            if (*path != 0) return FAT12_ERR_NO_FILE;
            *out_parent = cur;
            memcpy(out_name11, comp11, 11);
            if (out_entry) memset(out_entry, 0, 32);
            return 0;
        }

        if (*path == 0) {
            *out_parent = cur;
            memcpy(out_name11, comp11, 11);
            if (out_entry) memcpy(out_entry, e, 32);
            return 1;
        }

        if (!(e[11] & 0x10)) return FAT12_ERR_NO_FILE;
        uint16_t nc = (uint16_t)e[26] | ((uint16_t)e[27] << 8);
        if (nc < 2 || nc >= 0xFF8) return FAT12_ERR_NO_FILE;
        cur = nc;
    }
}

/* ---------- 挂载 ---------- */

int fat12_mount(void) {
    mounted = 0;
    cwd_cluster = 0;
    cwd_path[0] = 0;

    if (read_sector(0, sec_buf) != ATA_OK) return FAT12_ERR_IO;
    struct bpb* b = (struct bpb*)sec_buf;
    if (b->bytes_per_sector != 512 || b->sectors_per_cluster == 0 ||
        b->sectors_per_fat == 0 || b->root_entries == 0)
        return FAT12_ERR_NO_FAT;

    layout.fat_start           = b->reserved_sectors;
    layout.fat_sectors         = b->sectors_per_fat;
    layout.num_fats            = b->num_fats;
    layout.sectors_per_cluster = b->sectors_per_cluster;
    layout.root_start          = layout.fat_start + b->num_fats * b->sectors_per_fat;
    layout.root_sectors        = ((b->root_entries * 32) + 511) / 512;
    layout.data_start          = layout.root_start + layout.root_sectors;

    uint32_t total = b->total_sectors16 ? b->total_sectors16 : b->total_sectors32;
    layout.cluster_count = (total - layout.data_start) / b->sectors_per_cluster;

    if (layout.fat_sectors * 512 > sizeof(fat_buf)) return FAT12_ERR_NO_FAT;
    for (uint32_t i = 0; i < layout.fat_sectors; i++)
        if (read_sector(layout.fat_start + i, fat_buf + i * 512) != ATA_OK)
            return FAT12_ERR_IO;

    mounted = 1;
    return FAT12_OK;
}

int fat12_mounted(void) { return mounted; }
const fat12_layout_t* fat12_get_layout(void) { return &layout; }

/* ---------- 目录命令 ---------- */

int fat12_ls(const char* path) {
    if (!mounted) return FAT12_ERR_NOT_MOUNTED;

    uint16_t dir_cluster = cwd_cluster;

    if (path && *path) {
        uint16_t parent; uint8_t name11[11]; uint8_t entry[32];
        int r = path_resolve(path, &parent, name11, entry);
        if (r <= 0) return r < 0 ? r : FAT12_ERR_NO_FILE;
        if (!(entry[11] & 0x10)) return FAT12_ERR_NOT_DIR;
        dir_cluster = (uint16_t)entry[26] | ((uint16_t)entry[27] << 8);
    }

    char cwdbuf[256];
    fat12_getcwd(cwdbuf, sizeof(cwdbuf));
    kprintf("Directory of %s:\n", cwdbuf);
    kprintf("-----------------------------------\n");

    int count = 0;
    uint32_t max_sec = (dir_cluster == 0)
        ? layout.root_sectors
        : layout.sectors_per_cluster;
    int stop = 0;
    for (uint32_t s = 0; s < max_sec && !stop; s++) {
        uint32_t lba = (dir_cluster == 0)
            ? layout.root_start + s
            : layout.data_start + (dir_cluster - 2) * layout.sectors_per_cluster + s;
        if (read_sector(lba, sec_buf) != ATA_OK) break;
        for (int i = 0; i < 16; i++) {
            uint8_t* e = sec_buf + i * 32;
            if (e[0] == 0x00) { stop = 1; break; }
            if (e[0] == 0xE5) continue;
            if (e[11] & 0x08) continue;
            if (e[0] == '.' && (e[1] == ' ' || e[1] == '.')) continue;

            char name[13];
            format_83(e, name);
            uint32_t size = (uint32_t)e[28] | ((uint32_t)e[29] << 8)
                          | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
            uint16_t cluster = (uint16_t)e[26] | ((uint16_t)e[27] << 8);
            int is_dir = (e[11] & 0x10) != 0;

            kprintf("  %-12s %s %8u  start=%u\n",
                    name, is_dir ? "<DIR> " : "      ",
                    (unsigned int)size, (unsigned int)cluster);
            count++;
        }
    }
    if (count == 0) kprintf("  (empty)\n");
    kprintf("-----------------------------------\n");
    kprintf("%d entries\n", count);
    return FAT12_OK;
}

int fat12_mkdir(const char* path) {
    if (!mounted) return FAT12_ERR_NOT_MOUNTED;

    uint16_t parent; uint8_t name11[11]; uint8_t entry[32];
    int r = path_resolve(path, &parent, name11, entry);
    if (r < 0) return r;
    if (r == 1) return FAT12_ERR_EXISTS;

    uint32_t c = fat12_alloc_cluster();
    if (c == 0) return FAT12_ERR_FULL;

    /* 把新簇的 FAT 表项标记为 EOF（已用） */
    fat12_set((uint16_t)c, 0xFFF);

    /* 初始化新簇：. / .. */
    memset(sec_buf, 0, 512);
    uint8_t* dot = sec_buf;
    memset(dot, ' ', 11); dot[0] = '.';
    dot[11] = 0x10;
    dot[26] = (uint8_t)(c & 0xFF); dot[27] = (uint8_t)(c >> 8);

    uint8_t* dotdot = sec_buf + 32;
    memset(dotdot, ' ', 11); dotdot[0] = '.'; dotdot[1] = '.';
    dotdot[11] = 0x10;
    dotdot[26] = (uint8_t)(parent & 0xFF); dotdot[27] = (uint8_t)(parent >> 8);

    uint32_t lba = layout.data_start + (c - 2) * layout.sectors_per_cluster;
    if (write_sector(lba, sec_buf) != ATA_OK) {
        fat12_set(c, 0);
        return FAT12_ERR_IO;
    }

    r = dir_add(parent, name11, 0x10, (uint16_t)c, 0);
    if (r != FAT12_OK) {
        fat12_set(c, 0);
        return r;
    }
    fat12_flush_fat();
    return FAT12_OK;
}

int fat12_chdir(const char* path) {
    if (!mounted) return FAT12_ERR_NOT_MOUNTED;

    if (!path || !*path || strcmp(path, "/") == 0) {
        cwd_cluster = 0;
        cwd_path[0] = 0;
        return FAT12_OK;
    }

    /* ★ 特判：已经在根目录，"cd .." 保持根 */
    if (cwd_cluster == 0 && strcmp(path, "..") == 0) {
        cwd_path[0] = 0;
        return FAT12_OK;
    }

    uint16_t parent = 0;
    uint8_t  name11[11] = {0};
    uint8_t  entry[32]  = {0};
    int r = path_resolve(path, &parent, name11, entry);
    if (r < 0) return r;
    if (r == 0) return FAT12_ERR_NO_FILE;
    if (!(entry[11] & 0x10)) return FAT12_ERR_NOT_DIR;

    cwd_cluster = (uint16_t)entry[26] | ((uint16_t)entry[27] << 8);

    if (strcmp(path, "..") == 0) {
        int n = (int)strlen(cwd_path);
        while (n > 0 && cwd_path[n - 1] != '/') n--;
        if (n > 0) n--;
        cwd_path[n] = 0;
        return FAT12_OK;
    }

    if (path[0] == '/') {
        int n = 0;
        path++;
        while (*path && n < 250) cwd_path[n++] = *path++;
        cwd_path[n] = 0;
    } else {
        int n = (int)strlen(cwd_path);
        if (n > 0 && n < 250) cwd_path[n++] = '/';
        while (*path && n < 250) cwd_path[n++] = *path++;
        cwd_path[n] = 0;
    }
    return FAT12_OK;
}

void fat12_getcwd(char* buf, int size) {
    if (size < 2) { if (size) buf[0] = 0; return; }
    if (cwd_cluster == 0) {
        buf[0] = '/';
        buf[1] = 0;
        return;
    }
    int n = 0;
    buf[n++] = '/';
    for (int i = 0; cwd_path[i] && n < size - 1; i++)
        buf[n++] = cwd_path[i];
    buf[n] = 0;
}

/* ---------- 文件操作 ---------- */

int fat12_cat(const char* path) {
    if (!mounted) return FAT12_ERR_NOT_MOUNTED;

    uint16_t parent; uint8_t name11[11]; uint8_t entry[32];
    int r = path_resolve(path, &parent, name11, entry);
    if (r <= 0) return r < 0 ? r : FAT12_ERR_NO_FILE;
    if (entry[11] & 0x10) return FAT12_ERR_IS_DIR;

    uint32_t size = (uint32_t)entry[28] | ((uint32_t)entry[29] << 8)
                  | ((uint32_t)entry[30] << 16) | ((uint32_t)entry[31] << 24);
    uint16_t c = (uint16_t)entry[26] | ((uint16_t)entry[27] << 8);
    uint32_t remain = size;

    while (remain > 0 && c >= 2 && c < 0xFF8) {
        uint32_t lba = layout.data_start + (c - 2) * layout.sectors_per_cluster;
        for (uint8_t s = 0; s < layout.sectors_per_cluster && remain > 0; s++) {
            if (read_sector(lba + s, sec_buf) != ATA_OK) return FAT12_ERR_IO;
            uint32_t chunk = remain > 512 ? 512 : remain;
            for (uint32_t i = 0; i < chunk; i++)
                terminal_putchar((char)sec_buf[i]);
            remain -= chunk;
        }
        if (remain == 0) break;
        c = fat12_get(c);
    }
    return FAT12_OK;
}

int fat12_write(const char* path, const uint8_t* data, uint32_t size) {
    if (!mounted) return FAT12_ERR_NOT_MOUNTED;

    uint16_t parent; uint8_t name11[11]; uint8_t entry[32];
    int r = path_resolve(path, &parent, name11, entry);
    if (r < 0) return r;

    if (r == 1 && (entry[11] & 0x10)) return FAT12_ERR_IS_DIR;

    /* 释放旧簇链 */
    if (r == 1) {
        uint16_t c = (uint16_t)entry[26] | ((uint16_t)entry[27] << 8);
        while (c >= 2 && c < 0xFF8) {
            uint16_t next = fat12_get(c);
            fat12_set(c, 0);
            c = next;
        }
    }

    uint16_t first = 0, prev = 0;
    uint32_t remain = size;
    const uint8_t* src = data;

    while (remain > 0) {
        uint16_t c = (uint16_t)fat12_alloc_cluster();
        if (c == 0) {
            if (first) {
                uint16_t cc = first;
                while (cc >= 2 && cc < 0xFF8) {
                    uint16_t nx = fat12_get(cc);
                    fat12_set(cc, 0);
                    cc = nx;
                }
            }
            return FAT12_ERR_FULL;
        }
        if (!first) first = c;
        if (prev)   fat12_set(prev, c);

        uint32_t lba = layout.data_start + (c - 2) * layout.sectors_per_cluster;
        for (uint8_t s = 0; s < layout.sectors_per_cluster && remain > 0; s++) {
            memset(sec_buf, 0, 512);
            uint32_t chunk = remain > 512 ? 512 : remain;
            memcpy(sec_buf, src, chunk);
            if (write_sector(lba + s, sec_buf) != ATA_OK) return FAT12_ERR_IO;
            src += chunk;
            remain -= chunk;
        }
        prev = c;
    }
    if (prev) fat12_set(prev, 0xFFF);

    /* 更新或添加目录项 */
    if (r == 1) {
        uint8_t e2[32];
        uint32_t lba; int idx;
        if (!dir_find(parent, name11, e2, &lba, &idx)) return FAT12_ERR_IO;
        e2[26] = (uint8_t)(first & 0xFF);
        e2[27] = (uint8_t)(first >> 8);
        e2[28] = (uint8_t)(size & 0xFF);
        e2[29] = (uint8_t)((size >> 8) & 0xFF);
        e2[30] = (uint8_t)((size >> 16) & 0xFF);
        e2[31] = (uint8_t)((size >> 24) & 0xFF);
        if (dir_update(lba, idx, e2) != FAT12_OK) return FAT12_ERR_IO;
    } else {
        r = dir_add(parent, name11, 0x20, first, size);
        if (r != FAT12_OK) return r;
    }
    fat12_flush_fat();
    return FAT12_OK;
}

int fat12_delete(const char* path) {
    if (!mounted) return FAT12_ERR_NOT_MOUNTED;

    uint16_t parent; uint8_t name11[11]; uint8_t entry[32];
    int r = path_resolve(path, &parent, name11, entry);
    if (r <= 0) return r < 0 ? r : FAT12_ERR_NO_FILE;
    if (entry[11] & 0x10) return FAT12_ERR_IS_DIR;

    uint16_t c = (uint16_t)entry[26] | ((uint16_t)entry[27] << 8);
    while (c >= 2 && c < 0xFF8) {
        uint16_t next = fat12_get(c);
        fat12_set(c, 0);
        c = next;
    }

    uint8_t e2[32];
    uint32_t lba; int idx;
    if (!dir_find(parent, name11, e2, &lba, &idx)) return FAT12_ERR_IO;
    e2[0] = 0xE5;
    if (dir_update(lba, idx, e2) != FAT12_OK) return FAT12_ERR_IO;

    fat12_flush_fat();
    return FAT12_OK;
}

void fat12_dump_bpb(void) {
    if (!mounted) { kprintf("Not mounted.\n"); return; }
    kprintf("Layout:\n");
    kprintf("  FAT start   = LBA %u (%u sectors)\n", layout.fat_start, layout.fat_sectors);
    kprintf("  Root start  = LBA %u (%u sectors)\n", layout.root_start, layout.root_sectors);
    kprintf("  Data start  = LBA %u\n", layout.data_start);
    kprintf("  Clusters    = %u (spc=%u)\n",
            layout.cluster_count, layout.sectors_per_cluster);
    kprintf("  CWD cluster = %u\n", cwd_cluster);
}
