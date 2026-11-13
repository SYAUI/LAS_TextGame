/* fat12.h - FAT12 文件系统驱动（支持子目录） */
#ifndef SODA_FAT12_H
#define SODA_FAT12_H

#include "types.h"

#define FAT12_MAX_NAME       13
#define FAT12_SECTOR_SIZE    512

typedef struct {
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint8_t  sectors_per_cluster;
    uint8_t  num_fats;
} fat12_layout_t;

typedef struct {
    char     name[13];
    uint8_t  attr;
    uint16_t cluster;
    uint32_t size;
} fat12_entry_t;

#define FAT12_OK              0
#define FAT12_ERR_NOT_MOUNTED -1
#define FAT12_ERR_NO_FAT      -2
#define FAT12_ERR_NO_FILE     -3
#define FAT12_ERR_IO          -4
#define FAT12_ERR_FULL        -5
#define FAT12_ERR_EXISTS      -6
#define FAT12_ERR_INVALID     -7
#define FAT12_ERR_NOT_DIR     -8
#define FAT12_ERR_IS_DIR      -9

int  fat12_mount(void);
int  fat12_mounted(void);
const fat12_layout_t* fat12_get_layout(void);

/* 目录操作 */
int  fat12_ls(const char* path);                /* NULL 或 "" = CWD */
int  fat12_mkdir(const char* path);
int  fat12_chdir(const char* path);
void fat12_getcwd(char* buf, int size);

/* 文件操作 */
int  fat12_cat(const char* path);
int  fat12_write(const char* path, const uint8_t* data, uint32_t size);
int  fat12_delete(const char* path);

void fat12_dump_bpb(void);

#endif
