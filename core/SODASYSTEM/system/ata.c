/* ata.c - ATA PIO 驱动（LBA28，主通道主设备） */
#include "ata.h"
#include "io.h"
#include "lib.h"

/* 主 ATA 通道端口 */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_FEATURES    0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_ALT_STATUS  0x3F6

/* 状态位 */
#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DF       0x20
#define ATA_SR_DSC      0x10
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

/* 命令 */
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC

static int      ata_present   = 0;
static uint32_t ata_sectors   = 0;
static char     ata_model[41];

/* 400ns 延迟：读 status 端口 4 次 */
static void ata_delay400(void) {
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
}

/* 等待 BSY 清除，返回最终 status，超时返回 0 */
static uint8_t ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ATA_SR_BSY)) return s;
    }
    return 0;
}

/* 等待 DRQ 置位，返回 status */
static uint8_t ata_wait_drq(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ATA_SR_ERR) return 0;
        if (s & ATA_SR_DF)  return 0;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return s;
    }
    return 0;
}

/* ---- IDENTIFY ---- */
static int ata_identify(void) {
    outb(ATA_DRIVE_HEAD, 0xA0);          /* 主设备 */
    ata_delay400();

    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay400();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) return ATA_ERR_NO_DISK;      /* 没有设备 */

    /* 等待 BSY 清除 */
    if (!ata_wait_not_busy()) return ATA_ERR_TIMEOUT;
    if (!ata_wait_drq())      return ATA_ERR_TIMEOUT;

    uint16_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = inw(ATA_DATA);

    /* word 0x00 = 0x0040 表示 ATA 设备 */
    if (buf[0] != 0x0040) return ATA_ERR_DEVICE;

    /* 解析扇区总数（LBA28：words 60-61） */
    ata_sectors = (uint32_t)buf[60] | ((uint32_t)buf[61] << 16);

    /* 解析型号（words 27-46，注意 ATA 是大端字节序） */
    for (int i = 0; i < 40; i += 2) {
        ata_model[i]     = (char)(buf[27 + i / 2] >> 8);
        ata_model[i + 1] = (char)(buf[27 + i / 2] & 0xFF);
    }
    ata_model[40] = 0;

    /* 去除尾部空格 */
    for (int i = 39; i >= 0 && ata_model[i] == ' '; i--) ata_model[i] = 0;

    ata_present = 1;
    return ATA_OK;
}

int ata_init(void) {
    ata_present = 0;
    ata_sectors = 0;
    ata_model[0] = 0;
    return ata_identify();
}

int ata_is_present(void) { return ata_present; }
uint32_t ata_get_sector_count(void) { return ata_sectors; }
const char* ata_get_model(void) { return ata_model; }

/* ---- 读一个扇区 ---- */
int ata_read_sector(uint32_t lba, uint8_t* buf) {
    if (!ata_present) return ATA_ERR_NO_DISK;
    if (lba >= ata_sectors) return ATA_ERR_DEVICE;

    /* 选择主设备 + LBA 高 4 位 */
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay400();

    outb(ATA_FEATURES, 0);
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ);
    ata_delay400();

    if (!ata_wait_drq()) return ATA_ERR_TIMEOUT;

    uint16_t* p = (uint16_t*)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);

    return ATA_OK;
}

/* ---- 写一个扇区 ---- */
int ata_write_sector(uint32_t lba, const uint8_t* buf) {
    if (!ata_present) return ATA_ERR_NO_DISK;
    if (lba >= ata_sectors) return ATA_ERR_DEVICE;

    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay400();

    outb(ATA_FEATURES, 0);
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE);
    ata_delay400();

    if (!ata_wait_drq()) return ATA_ERR_TIMEOUT;

    const uint16_t* p = (const uint16_t*)buf;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);

    /* 刷新写缓存 */
    outb(ATA_COMMAND, 0xE7);
    ata_delay400();
    ata_wait_not_busy();

    return ATA_OK;
}

/* ---- 读多个扇区（未优化，逐扇区读） ---- */
int ata_read_sectors(uint32_t lba, uint8_t count, uint8_t* buf) {
    for (uint8_t i = 0; i < count; i++) {
        int r = ata_read_sector(lba + i, buf + i * ATA_SECTOR_SIZE);
        if (r != ATA_OK) return r;
    }
    return ATA_OK;
}