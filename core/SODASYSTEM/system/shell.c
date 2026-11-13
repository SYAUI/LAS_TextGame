/* shell.c - 命令行解释器 */
#include "shell.h"
#include "kernel.h"
#include "lib.h"
#include "keyboard.h"
#include "pit.h"
#include "ata.h"
#include "fat12.h"

#define PROMPT      "SODA> "
#define PROMPT_LEN  6
#define LINE_MAX    74
#define MAX_ARGS    16
#define HIST_MAX    16

static char  line_buf[LINE_MAX + 1];
static int   line_len;
static int   cursor_pos;
static int   prompt_row;

static char  history[HIST_MAX][LINE_MAX + 1];
static int   hist_count;
static int   hist_index;
static char  stash[LINE_MAX + 1];     /* 浏览历史时暂存当前输入 */

/* ---------- 重绘当前命令行 ---------- */
static void render_line(void) {
    terminal_clear_row((size_t)prompt_row);
    terminal_write_at((size_t)prompt_row, 0, PROMPT);
    terminal_write_at((size_t)prompt_row, PROMPT_LEN, line_buf);
    terminal_set_cursor((size_t)prompt_row,
                        (size_t)(PROMPT_LEN + cursor_pos));
}

/* ---------- 历史操作 ---------- */
static void hist_save(const char* cmd) {
    if (cmd[0] == 0) return;
    if (hist_count > 0 && strcmp(history[hist_count - 1], cmd) == 0) return;

    if (hist_count == HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++)
            memcpy(history[i - 1], history[i], LINE_MAX + 1);
        hist_count = HIST_MAX - 1;
    }
    int len = (int)strlen(cmd);
    if (len > LINE_MAX) len = LINE_MAX;
    memcpy(history[hist_count], cmd, (size_t)len);
    history[hist_count][len] = 0;
    hist_count++;
}

static void hist_prev(void) {
    if (hist_count == 0) return;
    if (hist_index == -1) {
        memcpy(stash, line_buf, LINE_MAX + 1);
        hist_index = hist_count - 1;
    } else if (hist_index > 0) {
        hist_index--;
    } else {
        return;
    }
    memcpy(line_buf, history[hist_index], LINE_MAX + 1);
    line_len    = (int)strlen(line_buf);
    cursor_pos  = line_len;
    render_line();
}

static void hist_next(void) {
    if (hist_index == -1) return;
    if (hist_index < hist_count - 1) {
        hist_index++;
        memcpy(line_buf, history[hist_index], LINE_MAX + 1);
    } else {
        hist_index = -1;
        memcpy(line_buf, stash, LINE_MAX + 1);
    }
    line_len   = (int)strlen(line_buf);
    cursor_pos = line_len;
    render_line();
}

/* ---------- 插入/删除字符 ---------- */
static void insert_char(char c) {
    if (line_len >= LINE_MAX) return;
    for (int i = line_len; i > cursor_pos; i--)
        line_buf[i] = line_buf[i - 1];
    line_buf[cursor_pos] = c;
    line_len++;
    cursor_pos++;
    line_buf[line_len] = 0;
    render_line();
}

static void delete_before(void) {
    if (cursor_pos == 0) return;
    for (int i = cursor_pos; i < line_len; i++)
        line_buf[i - 1] = line_buf[i];
    line_len--;
    cursor_pos--;
    line_buf[line_len] = 0;
    render_line();
}

static void delete_after(void) {
    if (cursor_pos >= line_len) return;
    for (int i = cursor_pos; i < line_len - 1; i++)
        line_buf[i] = line_buf[i + 1];
    line_len--;
    line_buf[line_len] = 0;
    render_line();
}

/* ---------- 命令实现 ---------- */
static void cmd_help(void) {
    kprintf("Built-in commands:\n");
    kprintf("  help                - show this help\n");
    kprintf("  clear / cls         - clear screen\n");
    kprintf("  echo <text>         - print text\n");
    kprintf("  uptime              - show system uptime\n");
    kprintf("  history             - show command history\n");
    kprintf("  ls [path]           - list directory (default: CWD)\n");
    kprintf("  cd <path>           - change directory (no arg = root)\n");
    kprintf("  pwd                 - print working directory\n");
    kprintf("  mkdir <path>        - create directory\n");
    kprintf("  cat <file>          - display file contents\n");
    kprintf("  write <file> <text> - create/overwrite file\n");
    kprintf("  rm <file>           - delete file\n");
    kprintf("  disk                - ATA info\n");
    kprintf("  hexdump <lba>       - dump raw sector\n");
    kprintf("  bpb                 - show FAT12 layout\n");
    kprintf("  halt                - halt the system\n");
}

static void cmd_history(void) {
    for (int i = 0; i < hist_count; i++)
        kprintf("%2d  %s\n", i + 1, history[i]);
}

static void cmd_clear(void) {
    terminal_clear();
    prompt_row = 0;
    terminal_write_at(0, 0, PROMPT);
    terminal_set_cursor(0, PROMPT_LEN);
    prompt_row = 0;
    /* shell 会用当前 prompt_row 渲染，见主循环 */
    /* 注意：这里 clear 之后的行号从 0 开始，主循环里的 prompt_row 已经更新 */
}

/* 分割参数，原地修改 line（把空格换成 0） */
static int split_args(char* line, char* argv[], int max) {
    int   argc = 0;
    char* p = line;
    while (*p == ' ') p++;
    if (!*p) return 0;
    while (*p && argc < max) {
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
        while (*p == ' ') p++;
    }
    return argc;
}

static void execute(char* line) {
    char* argv[MAX_ARGS];
    int argc = split_args(line, argv, MAX_ARGS);
    if (argc == 0) return;

    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
        return;
    }
    if (strcmp(argv[0], "clear") == 0 || strcmp(argv[0], "cls") == 0) {
        cmd_clear();
        return;
    }
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) kprintf(" ");
            kprintf("%s", argv[i]);
        }
        kprintf("\n");
        return;
    }
    if (strcmp(argv[0], "uptime") == 0) {
        pit_print_uptime();
        return;
    }
    if (strcmp(argv[0], "history") == 0) {
        cmd_history();
        return;
    }
    if (strcmp(argv[0], "halt") == 0 ||
        strcmp(argv[0], "poweroff") == 0 ||
        strcmp(argv[0], "shutdown") == 0) {
        kprintf("System halted. Use QEMU window close / Ctrl+Alt+Q.\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    /* ---- disk 命令 ---- */
    if (strcmp(argv[0], "disk") == 0) {
        if (!ata_is_present()) {
            kprintf("No ATA disk detected.\n");
            kprintf("Start QEMU with -drive format=raw,file=disk.img,if=ide,index=0\n");
            return;
        }
        kprintf("ATA Disk Information:\n");
        kprintf("  Model:   %s\n", ata_get_model());
        kprintf("  Sectors: %u (%.2u KB)\n",
                ata_get_sector_count(),
                ata_get_sector_count() * 512 / 1024);
        return;
    }

    /* ---- hexdump <lba> ---- */
    if (strcmp(argv[0], "hexdump") == 0 && argc >= 2) {
        if (!ata_is_present()) {
            kprintf("No ATA disk.\n");
            return;
        }
        uint32_t lba = 0;
        for (const char* p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("Invalid LBA: %s\n", argv[1]); return; }
            lba = lba * 10 + (uint32_t)(*p - '0');
        }
        static uint8_t sec[ATA_SECTOR_SIZE];
        int r = ata_read_sector(lba, sec);
        if (r != ATA_OK) {
            kprintf("Read failed (code %d).\n", r);
            return;
        }
        kprintf("Sector %u (0x%x):\n", lba, lba);
        for (int row = 0; row < 32; row++) {
            kprintf("%04x: ", row * 16);
            for (int col = 0; col < 16; col++) {
                kprintf("%02x ", sec[row * 16 + col]);
            }
            kprintf(" |");
            for (int col = 0; col < 16; col++) {
                uint8_t c = sec[row * 16 + col];
                kprintf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            kprintf("|\n");
        }
        return;
    }

    /* ---- diskwrite <lba> <text> ---- */
    if (strcmp(argv[0], "diskwrite") == 0 && argc >= 3) {
        if (!ata_is_present()) {
            kprintf("No ATA disk.\n");
            return;
        }
        uint32_t lba = 0;
        for (const char* p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("Invalid LBA: %s\n", argv[1]); return; }
            lba = lba * 10 + (uint32_t)(*p - '0');
        }
        static uint8_t sec[ATA_SECTOR_SIZE];
        memset(sec, 0, ATA_SECTOR_SIZE);
        /* 拼接 argv[2..] */
        int pos = 0;
        for (int i = 2; i < argc && pos < 500; i++) {
            if (i > 2 && pos < 500) sec[pos++] = ' ';
            for (const char* p = argv[i]; *p && pos < 500; p++)
                sec[pos++] = (uint8_t)*p;
        }
        int r = ata_write_sector(lba, sec);
        if (r != ATA_OK) {
            kprintf("Write failed (code %d).\n", r);
            return;
        }
        kprintf("Wrote %d bytes to sector %u.\n", pos, lba);
        return;
    }
    if (strcmp(argv[0], "ls") == 0 || strcmp(argv[0], "dir") == 0) {
        const char* path = (argc >= 2) ? argv[1] : NULL;
        int r = fat12_ls(path);
        if (r == FAT12_ERR_NO_FILE)      kprintf("Path not found.\n");
        else if (r == FAT12_ERR_NOT_DIR) kprintf("Not a directory.\n");
        else if (r == FAT12_ERR_NOT_MOUNTED) kprintf("FAT12 not mounted.\n");
        return;
    }
    
    if (strcmp(argv[0], "cd") == 0) {
        const char* path = (argc >= 2) ? argv[1] : "/";
        int r = fat12_chdir(path);
        if (r == FAT12_ERR_NO_FILE)      kprintf("Path not found.\n");
        else if (r == FAT12_ERR_NOT_DIR) kprintf("Not a directory.\n");
        else if (r == FAT12_ERR_NOT_MOUNTED) kprintf("FAT12 not mounted.\n");
        return;
    }
    
    if (strcmp(argv[0], "pwd") == 0) {
        char buf[256];
        fat12_getcwd(buf, sizeof(buf));
        kprintf("%s\n", buf);
        return;
    }
    
    if (strcmp(argv[0], "mkdir") == 0 || strcmp(argv[0], "md") == 0) {
        if (argc < 2) { kprintf("usage: mkdir <path>\n"); return; }
        int r = fat12_mkdir(argv[1]);
        if (r == FAT12_OK)                     kprintf("Directory created.\n");
        else if (r == FAT12_ERR_EXISTS)        kprintf("Already exists.\n");
        else if (r == FAT12_ERR_NO_FILE)       kprintf("Parent path not found.\n");
        else if (r == FAT12_ERR_FULL)          kprintf("Disk full.\n");
        else                                   kprintf("mkdir failed (%d).\n", r);
        return;
    }
    
    if (strcmp(argv[0], "cat") == 0) {
        if (argc < 2) { kprintf("usage: cat <file>\n"); return; }
        int r = fat12_cat(argv[1]);
        if (r == FAT12_OK)                     kprintf("\n");
        else if (r == FAT12_ERR_NO_FILE)       kprintf("File not found.\n");
        else if (r == FAT12_ERR_IS_DIR)        kprintf("Is a directory.\n");
        else if (r == FAT12_ERR_IO)            kprintf("I/O error.\n");
        return;
    }

    if (strcmp(argv[0], "write") == 0) {
        if (argc < 2) { kprintf("usage: write <file> [text...]\n"); return; }
        static uint8_t buf[1024];
        int len = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2 && len < (int)sizeof(buf) - 1) buf[len++] = ' ';
            for (const char* p = argv[i]; *p && len < (int)sizeof(buf) - 1; p++)
                buf[len++] = (uint8_t)*p;
        }
        int r = fat12_write(argv[1], buf, (uint32_t)len);
        if (r == FAT12_OK)                kprintf("Wrote %d bytes to %s.\n", len, argv[1]);
        else if (r == FAT12_ERR_FULL)     kprintf("Disk full.\n");
        else if (r == FAT12_ERR_IS_DIR)   kprintf("Is a directory.\n");
        else                              kprintf("Write failed (%d).\n", r);
        return;
    }

    if (strcmp(argv[0], "rm") == 0 || strcmp(argv[0], "del") == 0) {
        if (argc < 2) { kprintf("usage: rm <file>\n"); return; }
        int r = fat12_delete(argv[1]);
        if (r == FAT12_OK)                kprintf("Deleted %s.\n", argv[1]);
        else if (r == FAT12_ERR_NO_FILE)  kprintf("File not found.\n");
        else if (r == FAT12_ERR_IS_DIR)   kprintf("Is a directory.\n");
        else                              kprintf("Delete failed (%d).\n", r);
        return;
    }
    
    if (strcmp(argv[0], "bpb") == 0) {
        fat12_dump_bpb();
        return;
    }
    kprintf("Unknown command: %s\n", argv[0]);
    kprintf("Type 'help' for a list of commands.\n");
}

/* ---------- 主循环 ---------- */
void shell_run(void) {
    line_len    = 0;
    cursor_pos  = 0;
    line_buf[0] = 0;
    hist_count  = 0;
    hist_index  = -1;
    prompt_row  = (int)terminal_get_row();

    terminal_write_at((size_t)prompt_row, 0, PROMPT);
    terminal_set_cursor((size_t)prompt_row, PROMPT_LEN);

    for (;;) {
        int c = keyboard_getchar();
        if (c < 0) {
            __asm__ __volatile__("hlt");
            continue;
        }

        /* 普通可打印字符 */
        if (c >= 0x20 && c <= 0x7E) {
            insert_char((char)c);
            continue;
        }

        switch (c) {
            case '\n':
                /* 提交命令 */
                line_buf[line_len] = 0;
                terminal_set_cursor((size_t)prompt_row,
                                    (size_t)(PROMPT_LEN + line_len));
                kprintf("\n");
                hist_save(line_buf);
                execute(line_buf);
                line_len   = 0;
                cursor_pos = 0;
                line_buf[0] = 0;
                hist_index = -1;
                prompt_row = (int)terminal_get_row();
                terminal_write_at((size_t)prompt_row, 0, PROMPT);
                terminal_set_cursor((size_t)prompt_row, PROMPT_LEN);
                break;

            case '\b':
                delete_before();
                break;

            case KEY_UP:
                hist_prev();
                break;

            case KEY_DOWN:
                hist_next();
                break;

            case KEY_LEFT:
                if (cursor_pos > 0) {
                    cursor_pos--;
                    terminal_set_cursor((size_t)prompt_row,
                                        (size_t)(PROMPT_LEN + cursor_pos));
                }
                break;

            case KEY_RIGHT:
                if (cursor_pos < line_len) {
                    cursor_pos++;
                    terminal_set_cursor((size_t)prompt_row,
                                        (size_t)(PROMPT_LEN + cursor_pos));
                }
                break;

            case KEY_HOME:
                cursor_pos = 0;
                terminal_set_cursor((size_t)prompt_row, PROMPT_LEN);
                break;

            case KEY_END:
                cursor_pos = line_len;
                terminal_set_cursor((size_t)prompt_row,
                                    (size_t)(PROMPT_LEN + line_len));
                break;

            case KEY_DELETE:
                delete_after();
                break;
        }
    }
}
