/* ata.h - ATA PIO 磁盘驱动 */
#ifndef SODA_ATA_H
#define SODA_ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE  512

/* 返回值 */
#define ATA_OK            0
#define ATA_ERR_TIMEOUT  -1
#define ATA_ERR_DEVICE   -2
#define ATA_ERR_NO_DISK  -3

int         ata_init(void);
int         ata_read_sector(uint32_t lba, uint8_t* buf);
int         ata_write_sector(uint32_t lba, const uint8_t* buf);
int         ata_read_sectors(uint32_t lba, uint8_t count, uint8_t* buf);

uint32_t    ata_get_sector_count(void);
const char* ata_get_model(void);
int         ata_is_present(void);

#endif