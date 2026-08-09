#include "shell.h"
#include "fat32.h"
#include "console.h"
#include "serial.h"
#include "keyboard.h"
#include "string.h"
#include "kheap.h"
#include "pmm.h"
#include "vmm.h"
#include "elf64.h"
#include "highlight.h"
#include "rtc.h"
#include "pci.h"
#include "pit.h"
#include "task.h"
#include "process.h"
#include "editor.h"
#include "fb.h"
#include "panic.h"
#include "io.h"
#include <stdarg.h>
#include <stddef.h>

__attribute__((noreturn)) extern void halt_forever(void); /* kernel/arch/x86_64/entry.S */

#define MAX_LINE       256
#define MAX_ARGS       16
#define MAX_PATH_DEPTH 32
#define MAX_USERS      16
#define FILE_BUF_SIZE  (64u * 1024)

static uint32_t g_cwd_cluster;
static char g_path_stack[MAX_PATH_DEPTH][13];
static int g_path_depth = 0;

#define HISTORY_SIZE 16
static char g_history[HISTORY_SIZE][MAX_LINE];
static int g_history_count = 0;
static int g_history_next = 0;

static void history_add(const char *line) {
    if (line[0] == '\0') {
        return; /* пустые строки в историю не пишем */
    }
    strcpy_safe(g_history[g_history_next], MAX_LINE, line);
    g_history_next = (g_history_next + 1) % HISTORY_SIZE;
    if (g_history_count < HISTORY_SIZE) {
        g_history_count++;
    }
}


/* 
    :: Здесь будут указаны все команды 

    help -> cmd_help
    clear -> cmd_clear
    echo -> cmd_echo
    pwd -> cmd_pwd
    ls -> cmd_ls
    cd -> cmd_cd
    mkdir -> cmd_mkdir
    rmdir -> cmd_rmdir
    touch -> cmd_touch
    rm -> cmd_rm
    cp -> cmd_cp
    mv -> cmd_mv
    cat -> cmd_cat
    wc -> cmd_wc
    head -> cmd_head
    tail -> cmd_tail
    grep -> cmd_grep
    hexdump -> cmd_hexdump
    sleep -> cmd_sleep
    history -> cmd_history
    nano -> cmd_nano
    whoami -> cmd_whoami
    users -> cmd_users
    useradd -> cmd_useradd
    su -> cmd_su
    sudo -> cmd_sudo
    uname -> cmd_uname
    date -> cmd_date
    lspci -> cmd_lspci
    loadbin -> cmd_loadbin
    free -> cmd_free
    df -> cmd_df
    ps -> cmd_ps
    spawn -> cmd_spawn
    sched -> cmd_sched
    tasks -> cmd_tasks
    dmesg -> cmd_dmesg
    sh -> cmd_sh
    run -> cmd_run
    reboot -> cmd_reboot
    halt -> cmd_halt


*/

static int g_is_root = 0;
static char g_username[32] = "guest";

static char g_users[MAX_USERS][32];
static int g_user_count = 0;

/* --- output to both the graphical console (if any) and serial --- */

static void shell_write(const char *s) {
    console_write(s);
    serial_write(s);
}

static void shell_printf(const char *fmt, ...) {
    va_list a1, a2;
    va_start(a1, fmt);
    va_copy(a2, a1);
    serial_vprintf(fmt, a1);
    console_vprintf(fmt, a2);
    va_end(a1);
    va_end(a2);
}

/* Deterministic per-username color, so different users are visually
   distinct in the prompt and in `users` -- cosmetic only, not a real
   permission indicator. */
static uint32_t username_color(const char *name) {
    uint32_t hash = 5381;
    for (const char *p = name; *p; p++) {
        hash = hash * 33u + (uint8_t)*p;
    }
    static const uint8_t palette[6][3] = {
        {0x50, 0xC0, 0xFF}, {0x50, 0xFF, 0x90}, {0xFF, 0xC0, 0x50},
        {0xFF, 0x80, 0xC0}, {0xB0, 0x90, 0xFF}, {0x90, 0xFF, 0xE0},
    };
    const uint8_t *c = palette[hash % 6];
    return fb_make_color(c[0], c[1], c[2]);
}

static void print_prompt(void) {
    uint32_t color = username_color(g_username);
    console_write_len_color(g_username, strlen(g_username), color);
    serial_write(g_username);
    shell_write("@apexos:/");
    for (int i = 0; i < g_path_depth; i++) {
        shell_printf("%s/", g_path_stack[i]);
    }
    shell_write(g_is_root ? "# " : "$ ");
}

/* --- line input with backspace editing + Up/Down history recall --- */
static void tasks_run(void); /* определена ниже, вызывается отсюда через Ctrl+Shift+Esc */

static void erase_displayed(size_t len) {
    for (size_t i = 0; i < len; i++) {
        console_backspace();
        serial_write("\b \b");
    }
}

static void display_text(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        console_putc(s[i]);
        serial_putc(s[i]);
    }
}

static void read_line(char *buf, size_t max_len) {
    size_t len = 0;
    buf[0] = '\0';
    int browse = -1; /* -1 = свежий ввод, 0 = самая последняя команда истории, ... */

    while (1) {
        int ev = keyboard_read_key();
        if (ev == KEY_NONE) {
            __asm__ volatile ("hlt");
            continue;
        }

        if (ev == '\n' || ev == '\r') {
            console_putc('\n');
            serial_putc('\n');
            buf[len] = '\0';
            return;
        }
        if (ev == '\b') {
            if (len > 0) {
                len--;
                console_backspace();
                serial_write("\b \b");
            }
            browse = -1;
            continue;
        }
        if (ev == KEY_CTRL_SHIFT_ESC) {
            erase_displayed(len);
            tasks_run();
            display_text(buf, len);
            continue;
        }
        if (ev == KEY_UP) {
            if (browse + 1 >= g_history_count) {
                continue; /* дошли до конца истории (или она пуста) */
            }
            browse++;
            int idx = ((g_history_next - 1 - browse) % HISTORY_SIZE + HISTORY_SIZE * 2) % HISTORY_SIZE;
            erase_displayed(len);
            size_t hlen = strlen(g_history[idx]);
            if (hlen >= max_len) hlen = max_len - 1;
            memcpy(buf, g_history[idx], hlen);
            buf[hlen] = '\0';
            len = hlen;
            display_text(buf, len);
            continue;
        }
        if (ev == KEY_DOWN) {
            if (browse < 0) {
                continue;
            }
            browse--;
            erase_displayed(len);
            if (browse < 0) {
                len = 0;
                buf[0] = '\0';
            } else {
                int idx = ((g_history_next - 1 - browse) % HISTORY_SIZE + HISTORY_SIZE * 2) % HISTORY_SIZE;
                size_t hlen = strlen(g_history[idx]);
                if (hlen >= max_len) hlen = max_len - 1;
                memcpy(buf, g_history[idx], hlen);
                buf[hlen] = '\0';
                len = hlen;
                display_text(buf, len);
            }
            continue;
        }
        if (ev >= 32 && ev < 127 && len + 1 < max_len) {
            buf[len++] = (char)ev;
            console_putc((char)ev);
            serial_putc((char)ev);
            browse = -1;
            continue;
        }
        /* Left/Right/Home/End/Delete/F2/Ctrl+... в обычном приглашении
           shell честно не поддержаны (в отличие от редактора) — просто
           игнорируются, а не притворяются, что что-то делают. */
    }
}

/* --- split a line into argv (no quoting -- an honest simplification) --- */
static int tokenize(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < max_args) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ') p++;
        if (*p == ' ') {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

/* --- directory navigation --- */

static void path_push(const char *name) {
    if (g_path_depth < MAX_PATH_DEPTH) {
        size_t i = 0;
        while (name[i] != '\0' && i < sizeof(g_path_stack[0]) - 1) {
            g_path_stack[g_path_depth][i] = name[i];
            i++;
        }
        g_path_stack[g_path_depth][i] = '\0';
        g_path_depth++;
    }
}

static void path_pop(void) {
    if (g_path_depth > 0) {
        g_path_depth--;
    }
}

/* --- commands --- */

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write(
        "ApexOS shell commands:\n"
        "  help                 - this text\n"
        "  clear                - clear the screen (serial log untouched)\n"
        "  echo <text>          - print text\n"
        "  pwd                  - current directory\n"
        "  ls                   - list files/directories\n"
        "  cd <name|..>         - change directory\n"
        "  mkdir <name>         - create a directory\n"
        "  rmdir <name>         - remove an EMPTY directory\n"
        "  touch <name>         - create an empty file\n"
        "  rm <name>            - delete a file\n"
        "  cp <src> <dst>       - copy a file (same directory)\n"
        "  mv <src> <dst>       - rename a file (same directory)\n"
        "  cat <name>           - print a file's contents (syntax-highlighted\n"
        "                         for .c/.h/.cpp/.hpp)\n"
        "  wc <name>            - line/word/byte count\n"
        "  head <name> [n]      - first n lines (default 10)\n"
        "  tail <name> [n]      - last n lines (default 10)\n"
        "  grep <pat> <name>    - print lines containing a substring\n"
        "  hexdump <name>       - hex + ASCII dump\n"
        "  sleep <seconds>      - pause using the PIT tick counter\n"
        "  history              - show recent commands (also: Up/Down arrows\n"
        "                         recall previous commands at the prompt)\n"
        "  lspci                - list PCI devices (read-only, no config writes;\n"
        "                         flags likely network/Wi-Fi controllers)\n"
        "  loadbin <file.bin>   - load a binary into dedicated, page-aligned,\n"
        "                         physically-addressable memory, print virtual +\n"
        "                         physical address. HONEST: infrastructure only --\n"
        "                         no driver exists yet that consumes this.\n"
        "  nano <name>          - full-screen editor: arrows move, F2 saves,\n"
        "                         Esc exits, Ctrl+R saves and runs (only\n"
        "                         works if the file is already an ELF64 --\n"
        "                         ApexOS has no C compiler, see `run` below)\n"
        "  whoami               - current user\n"
        "  users                - list known users (in-memory only)\n"
        "  useradd <name>       - add a user (memory only: no passwords, no\n"
        "                         real access control -- bookkeeping, not security)\n"
        "  su <name>            - switch the active (displayed) user\n"
        "  sudo                 - toggle root mode. HONEST: the kernel has no\n"
        "                         real permission enforcement anywhere (FAT32,\n"
        "                         memory, devices do not check an owner) --\n"
        "                         this only flags the prompt/whoami/useradd gate.\n"
        "  uname                - system info\n"
        "  date                 - current date/time from the CMOS RTC\n"
        "  free                 - physical memory + heap usage\n"
        "  df                   - FAT32 volume usage\n"
        "  ps                   - list running \"processes\" and kernel tasks\n"
        "  spawn                - create 2 demo kernel threads (see `dmesg` for\n"
        "                         interleaved [demo-a]/[demo-b] tick output)\n"
        "  sched on|off         - enable/disable timer-driven preemption of kernel\n"
        "                         threads. HONEST: only ring0 kernel threads are\n"
        "                         scheduled -- a `run`/Ctrl+R ring3 program still\n"
        "                         owns the whole CPU until it exits (no per-process\n"
        "                         address spaces yet, see earlier discussion).\n"
        "  tasks                - full-screen live system monitor (also: Ctrl+Shift+Esc\n"
        "                         opens it instantly from anywhere at the prompt). Esc exits.\n"
        "  dmesg                - show the captured boot/runtime log (everything\n"
        "                         ever printed via serial, kept in a 32 KiB ring buffer\n"
        "                         -- lets you see the boot log even with no serial cable)\n"
        "  sh <script>          - run a script: one command per line, `#` comments,\n"
        "                         VAR=value assignment, $VAR expansion in any argument.\n"
        "                         Also supports (ApexOS's OWN simplified syntax --\n"
        "                         NOT real bash, no semicolons needed):\n"
        "                           if [ -f FILE ] then ... fi\n"
        "                           if [ -d DIR ] then ... fi\n"
        "                           if [ -z $X ] / [ -n $X ] then ... fi\n"
        "                           if [ $X = y ] / [ $X != y ] then ... fi\n"
        "                           for VAR in a b c do ... done\n"
        "                         HONEST: no pipes, no quoting, no command exit-code\n"
        "                         tests (only `[ ... ]`), no while, no functions.\n"
        "  dmesg                - full boot/kernel log (captures everything ever\n"
        "                         written to the serial log, up to 32 KiB, even if\n"
        "                         there is no real serial port -- it's an in-memory\n"
        "                         ring buffer, not the physical UART)\n"
        "  run <name.elf>       - load and run an ELF64 binary in ring3. It now\n"
        "                         RETURNS to the shell when the program calls\n"
        "                         exit (via a manual context switch, not a real\n"
        "                         scheduler -- nothing else can run while it's\n"
        "                         executing, but you get your shell back after)\n"
        "  reboot               - reboot via the 8042 keyboard controller\n"
        "  halt                 - stop the CPU (safe to power off after)\n"
    );
}
/*
static void cmd__help_simple(int argc, char **argv) {
    shell_write("help, clear,");
}
*/

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        shell_write(argv[i]);
        if (i + 1 < argc) shell_write(" ");
    }
    shell_write("\n");
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("/");
    for (int i = 0; i < g_path_depth; i++) {
        shell_write(g_path_stack[i]);
        shell_write("/");
    }
    shell_write("\n");
}

static void ls_callback(const struct fat32_dirent *e, void *ctx) {
    (void)ctx;
    char name[13];
    fat32_83_to_display(e->name, name);
    if (e->attr & FAT32_ATTR_DIRECTORY) {
        shell_printf("  <DIR>     %s\n", name);
    } else {
        shell_printf("  %8lu  %s\n", (unsigned long)e->file_size, name);
    }
}

static void cmd_ls(int argc, char **argv) {
    (void)argc; (void)argv;
    fat32_list_dir(g_cwd_cluster, ls_callback, NULL);
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        shell_write("cd: missing directory name (or ..)\n");
        return;
    }
    if (strcmp(argv[1], "..") == 0) {
        if (g_path_depth == 0) {
            shell_write("cd: already at root\n");
            return;
        }
        struct fat32_dirent dotdot;
        char name83[FAT32_NAME_LEN];
        fat32_name_to_83("..", name83);
        if (!fat32_find_entry(g_cwd_cluster, name83, &dotdot)) {
            shell_write("cd: could not find \"..\" in the current directory (corrupted?)\n");
            return;
        }
        uint32_t parent = ((uint32_t)dotdot.first_cluster_high << 16) | dotdot.first_cluster_low;
        g_cwd_cluster = (parent == 0) ? fat32_root_cluster() : parent;
        path_pop();
        return;
    }

    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) {
        shell_write("cd: invalid name (short 8.3 names only: letters/digits/-/_)\n");
        return;
    }
    struct fat32_dirent e;
    if (!fat32_find_entry(g_cwd_cluster, name83, &e)) {
        shell_write("cd: not found\n");
        return;
    }
    if (!(e.attr & FAT32_ATTR_DIRECTORY)) {
        shell_write("cd: that is a file, not a directory\n");
        return;
    }
    g_cwd_cluster = ((uint32_t)e.first_cluster_high << 16) | e.first_cluster_low;
    path_push(argv[1]);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { shell_write("mkdir: missing name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("mkdir: invalid name\n"); return; }
    if (fat32_mkdir(g_cwd_cluster, name83) != 0) {
        shell_write("mkdir: failed (already exists, or no space left)\n");
    }
}

static void cmd_rmdir(int argc, char **argv) {
    if (argc < 2) { shell_write("rmdir: missing name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("rmdir: invalid name\n"); return; }
    if (fat32_rmdir(g_cwd_cluster, name83) != 0) {
        shell_write("rmdir: failed (not found, not a directory, or not empty)\n");
    }
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) { shell_write("touch: missing name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("touch: invalid name\n"); return; }
    struct fat32_dirent existing;
    if (fat32_find_entry(g_cwd_cluster, name83, &existing)) {
        return; /* already exists -- like real touch, leave content alone */
    }
    if (fat32_write_file(g_cwd_cluster, name83, "", 0) != 0) {
        shell_write("touch: could not create file (no space?)\n");
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { shell_write("rm: missing name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("rm: invalid name\n"); return; }
    if (fat32_delete_file(g_cwd_cluster, name83) != 0) {
        shell_write("rm: failed (not found, or it's a directory -- use rmdir)\n");
    }
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) { shell_write("cp: usage: cp <src> <dst>\n"); return; }
    char src83[FAT32_NAME_LEN], dst83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], src83) != 0 || fat32_name_to_83(argv[2], dst83) != 0) {
        shell_write("cp: invalid name\n");
        return;
    }
    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("cp: out of memory\n"); return; }
    uint32_t real_size = 0;
    if (fat32_read_file(g_cwd_cluster, src83, buf, FILE_BUF_SIZE, &real_size) != 0) {
        shell_write("cp: source not found (or it's a directory)\n");
        kfree(buf);
        return;
    }
    if (real_size > FILE_BUF_SIZE) {
        shell_write("cp: source larger than the copy buffer, truncating\n");
        real_size = FILE_BUF_SIZE;
    }
    if (fat32_write_file(g_cwd_cluster, dst83, buf, real_size) != 0) {
        shell_write("cp: failed to write destination (no space?)\n");
    }
    kfree(buf);
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { shell_write("mv: usage: mv <src> <dst>\n"); return; }
    char src83[FAT32_NAME_LEN], dst83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], src83) != 0 || fat32_name_to_83(argv[2], dst83) != 0) {
        shell_write("mv: invalid name\n");
        return;
    }
    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("mv: out of memory\n"); return; }
    uint32_t real_size = 0;
    if (fat32_read_file(g_cwd_cluster, src83, buf, FILE_BUF_SIZE, &real_size) != 0) {
        shell_write("mv: source not found (or it's a directory)\n");
        kfree(buf);
        return;
    }
    if (fat32_write_file(g_cwd_cluster, dst83, buf, real_size) != 0) {
        shell_write("mv: failed to write destination (no space?)\n");
        kfree(buf);
        return;
    }
    fat32_delete_file(g_cwd_cluster, src83);
    kfree(buf);
}

static int has_c_extension(const char *name) {
    size_t len = strlen(name);
    static const char *const exts[] = { ".c", ".h", ".cpp", ".hpp", NULL };
    for (int e = 0; exts[e] != NULL; e++) {
        size_t elen = strlen(exts[e]);
        if (len < elen) continue;
        const char *suffix = name + (len - elen);
        int match = 1;
        for (size_t i = 0; i < elen; i++) {
            char a = suffix[i], b = exts[e][i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { shell_write("cat: missing file name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("cat: invalid name\n"); return; }

    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("cat: out of memory\n"); return; }

    uint32_t real_size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE - 1, &real_size) != 0) {
        shell_write("cat: file not found (or it's a directory)\n");
        kfree(buf);
        return;
    }
    uint32_t shown = (real_size < FILE_BUF_SIZE - 1) ? real_size : FILE_BUF_SIZE - 1;
    buf[shown] = '\0';

    if (has_c_extension(argv[1])) {
        struct highlight_state hl_state;
        highlight_state_reset(&hl_state);
        char *line_start = buf;
        for (uint32_t i = 0; i <= shown; i++) {
            if (i == shown || buf[i] == '\n') {
                if (i == shown && line_start == buf + i) {
                    break; /* file already ended in '\n' -- nothing left to flush */
                }
                char saved = buf[i];
                buf[i] = '\0';
                highlight_print_c_line(&hl_state, line_start);
                console_putc_np('\n');
                serial_putc('\n');
                buf[i] = saved;
                line_start = buf + i + 1;
            }
        }
        console_present(); /* один раз на весь файл, не по разу на строку/токен */
    } else {
        shell_write(buf);
        if (shown > 0 && buf[shown - 1] != '\n') {
            shell_write("\n");
        }
    }

    if (real_size > shown) {
        shell_printf("[cat: file larger than buffer, showing first %lu of %lu bytes]\n",
                     (unsigned long)shown, (unsigned long)real_size);
    }
    kfree(buf);
}

static void cmd_nano(int argc, char **argv) {
    if (argc < 2) { shell_write("nano: missing file name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("nano: invalid name\n"); return; }
    editor_run(g_cwd_cluster, name83, argv[1]);
    console_clear(); /* возвращаемся из полноэкранного редактора в чистый shell-экран */
}

static void cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_printf("%s%s\n", g_username, g_is_root ? " (root)" : "");
}

static void cmd_users(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("guest\n"); /* всегда существует, встроенный пользователь по умолчанию */
    for (int i = 0; i < g_user_count; i++) {
        int current = (strcmp(g_users[i], g_username) == 0);
        console_write_len_color(g_users[i], strlen(g_users[i]), username_color(g_users[i]));
        serial_write(g_users[i]);
        shell_write(current ? " (current)\n" : "\n");
    }
}

static void cmd_useradd(int argc, char **argv) {
    if (argc < 2) { shell_write("useradd: missing username\n"); return; }
    if (!g_is_root) {
        shell_write("useradd: root required -- run `sudo` first\n");
        return;
    }
    if (g_user_count >= MAX_USERS) {
        shell_write("useradd: user list full (in-memory only)\n");
        return;
    }
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i], argv[1]) == 0) {
            shell_write("useradd: already exists\n");
            return;
        }
    }
    strcpy_safe(g_users[g_user_count], sizeof(g_users[0]), argv[1]);
    g_user_count++;
    shell_printf("useradd: added \"%s\" (HONEST: in-memory list only -- no passwords, "
                 "no real login; bookkeeping, not real multi-user support)\n", argv[1]);
}

static void cmd_su(int argc, char **argv) {
    if (argc < 2) { shell_write("su: usage: su <username>\n"); return; }
    if (strcmp(argv[1], "guest") == 0) {
        strcpy_safe(g_username, sizeof(g_username), "guest");
        return;
    }
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i], argv[1]) == 0) {
            strcpy_safe(g_username, sizeof(g_username), argv[1]);
            return;
        }
    }
    shell_write("su: unknown user (see `users`, or add one with `useradd`)\n");
}

static void cmd_sudo(int argc, char **argv) {
    (void)argc; (void)argv;
    g_is_root = !g_is_root;
    if (g_is_root) {
        shell_write("sudo: root mode ON. HONEST: the kernel has no real permission "
                     "enforcement anywhere (FAT32/memory/devices do not check an "
                     "owner) -- this only flags the prompt and the useradd gate, "
                     "not a real security boundary.\n");
    } else {
        shell_write("sudo: root mode off\n");
    }
}

static void print2(int v) {
    char buf[3];
    buf[0] = (char)('0' + (v / 10) % 10);
    buf[1] = (char)('0' + v % 10);
    buf[2] = '\0';
    shell_write(buf);
}

static void cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    struct rtc_time t;
    rtc_read(&t);
    shell_printf("%u-", (unsigned)t.year);
    print2(t.month);
    shell_write("-");
    print2(t.day);
    shell_write(" ");
    print2(t.hour);
    shell_write(":");
    print2(t.minute);
    shell_write(":");
    print2(t.second);
    shell_write(" (CMOS RTC -- timezone is whatever the firmware/BIOS has it set to)\n");
}

static void cmd_wc(int argc, char **argv) {
    if (argc < 2) { shell_write("wc: missing file name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("wc: invalid name\n"); return; }
    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("wc: out of memory\n"); return; }
    uint32_t size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE, &size) != 0) {
        shell_write("wc: file not found\n");
        kfree(buf);
        return;
    }
    uint32_t lines = 0, words = 0;
    int in_word = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
            in_word = 0;
        } else if (!in_word) {
            words++;
            in_word = 1;
        }
    }
    shell_printf("%u %u %u %s\n", lines, words, size, argv[1]);
    kfree(buf);
}

static void print_first_n_lines(const char *buf, uint32_t size, int n) {
    int count = 0;
    for (uint32_t i = 0; i < size && count < n; i++) {
        console_putc(buf[i]);
        serial_putc(buf[i]);
        if (buf[i] == '\n') count++;
    }
}

static void print_last_n_lines(const char *buf, uint32_t size, int n) {
    uint32_t idx = size;
    int count = 0;
    while (idx > 0) {
        idx--;
        if (buf[idx] == '\n') {
            count++;
            if (count > n) { idx++; break; }
        }
    }
    for (uint32_t i = idx; i < size; i++) {
        console_putc(buf[i]);
        serial_putc(buf[i]);
    }
}

static int parse_uint_arg(const char *s, int fallback) {
    if (s == NULL || *s == '\0') return fallback;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return fallback;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static void cmd_head(int argc, char **argv) {
    if (argc < 2) { shell_write("head: usage: head <file> [lines]\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("head: invalid name\n"); return; }
    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("head: out of memory\n"); return; }
    uint32_t size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE, &size) != 0) {
        shell_write("head: file not found\n");
        kfree(buf);
        return;
    }
    print_first_n_lines(buf, size, parse_uint_arg(argc >= 3 ? argv[2] : NULL, 10));
    kfree(buf);
}

static void cmd_tail(int argc, char **argv) {
    if (argc < 2) { shell_write("tail: usage: tail <file> [lines]\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("tail: invalid name\n"); return; }
    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("tail: out of memory\n"); return; }
    uint32_t size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE, &size) != 0) {
        shell_write("tail: file not found\n");
        kfree(buf);
        return;
    }
    print_last_n_lines(buf, size, parse_uint_arg(argc >= 3 ? argv[2] : NULL, 10));
    kfree(buf);
}

static int line_contains(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] == needle[i]) i++;
        if (i == nlen) return 1;
    }
    return 0;
}

static void cmd_grep(int argc, char **argv) {
    if (argc < 3) { shell_write("grep: usage: grep <pattern> <file>\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[2], name83) != 0) { shell_write("grep: invalid name\n"); return; }
    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("grep: out of memory\n"); return; }
    uint32_t size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE - 1, &size) != 0) {
        shell_write("grep: file not found\n");
        kfree(buf);
        return;
    }
    buf[size] = '\0';
    char *line_start = buf;
    for (uint32_t i = 0; i <= size; i++) {
        if (i == size || buf[i] == '\n') {
            char saved = buf[i];
            buf[i] = '\0';
            if (line_contains(line_start, argv[1])) {
                shell_write(line_start);
                shell_write("\n");
            }
            buf[i] = saved;
            line_start = buf + i + 1;
        }
    }
    kfree(buf);
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) { shell_write("hexdump: missing file name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("hexdump: invalid name\n"); return; }
    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("hexdump: out of memory\n"); return; }
    uint32_t size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE, &size) != 0) {
        shell_write("hexdump: file not found\n");
        kfree(buf);
        return;
    }
    for (uint32_t off = 0; off < size; off += 16) {
        shell_printf("%08x  ", off);
        uint32_t line_len = (size - off < 16) ? (size - off) : 16;
        for (uint32_t i = 0; i < 16; i++) {
            if (i < line_len) {
                shell_printf("%02x ", (unsigned)(unsigned char)buf[off + i]);
            } else {
                shell_write("   ");
            }
        }
        shell_write(" ");
        for (uint32_t i = 0; i < line_len; i++) {
            unsigned char c = (unsigned char)buf[off + i];
            char ch = (c >= 32 && c < 127) ? (char)c : '.';
            console_putc(ch);
            serial_putc(ch);
        }
        shell_write("\n");
    }
    kfree(buf);
}

static void cmd_sleep(int argc, char **argv) {
    if (argc < 2) { shell_write("sleep: usage: sleep <seconds>\n"); return; }
    int secs = parse_uint_arg(argv[1], -1);
    if (secs < 0) { shell_write("sleep: invalid number\n"); return; }
    uint64_t target = pit_get_ticks() + (uint64_t)secs * 100; /* PIT работает на 100 Гц (см. main.c) */
    while (pit_get_ticks() < target) {
        __asm__ volatile ("hlt");
    }
}

static void cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = g_history_count - 1; i >= 0; i--) {
        int idx = ((g_history_next - 1 - i) % HISTORY_SIZE + HISTORY_SIZE * 2) % HISTORY_SIZE;
        shell_printf("%3d  %s\n", g_history_count - i, g_history[idx]);
    }
}

static void lspci_callback(const struct pci_device *dev, void *ctx) {
    (void)ctx;
    shell_printf("%3u:%2u.%u  vendor=%4x device=%4x  class=%2x sub=%2x prog_if=%2x",
                 (unsigned)dev->bus, (unsigned)dev->slot, (unsigned)dev->func,
                 (unsigned)dev->vendor_id, (unsigned)dev->device_id,
                 (unsigned)dev->class_code, (unsigned)dev->subclass, (unsigned)dev->prog_if);

    /* Класс 0x02 = Network Controller. Подкласс 0x00 = Ethernet (проводной),
       0x80 = "Other" -- почти всегда именно так PCI классифицирует Wi-Fi
       карты (нет отдельного официального подкласса "wireless"). Это
       эвристика (правильные способы -- смотреть capability list на
       PCI_CAP_ID_MSI-X/специфику вендора), но для "что у меня вообще
       есть" достаточно точна на практике. */
    if (dev->class_code == 0x02) {
        if (dev->subclass == 0x00) {
            shell_write("  <-- wired Ethernet controller");
        } else if (dev->subclass == 0x80) {
            shell_write("  <-- likely Wi-Fi controller (PCI class 02/80, "
                        "\"Other\" network -- typical for wireless NICs)");
        } else {
            shell_write("  <-- network controller (other subclass)");
        }
    }
    shell_write("\n");
}

/* Отдельный virtual-диапазон для загруженных бинарников (прошивок и
   т.п.) -- bump-аллокатор, память не освобождается (прошивки живут,
   пока система работает). Физически отдельные страницы, не через
   kheap -- будущему драйверу для DMA нужен РЕАЛЬНЫЙ физический адрес
   каждой страницы (vmm_get_phys), а не то, что даёт общий аллокатор
   кучи. ЧЕСТНО: сейчас это только инфраструктура загрузки -- ничто
   ещё не потребляет загруженный бинарник (нет драйверов, которые
   умели бы что-то с ним сделать). */
#define BINARY_LOAD_VIRT_BASE 0xFFFFFFFFFC000000ull
static uint64_t g_binary_load_next = BINARY_LOAD_VIRT_BASE;

static void cmd_loadbin(int argc, char **argv) {
    if (argc < 2) {
        shell_write("loadbin: usage: loadbin <file.bin>\n"
                     "  Loads a binary file into dedicated, page-aligned, physically\n"
                     "  addressable memory. HONEST: this is infrastructure only -- nothing\n"
                     "  in ApexOS yet consumes a loaded binary (no chipset drivers exist).\n"
                     "  Useful for inspecting where a firmware blob would end up in memory.\n");
        return;
    }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("loadbin: invalid name\n"); return; }

    char *tmp = (char *)kmalloc(FILE_BUF_SIZE);
    if (tmp == NULL) { shell_write("loadbin: out of memory\n"); return; }
    uint32_t size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, tmp, FILE_BUF_SIZE, &size) != 0) {
        shell_write("loadbin: file not found\n");
        kfree(tmp);
        return;
    }

    uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    if (pages == 0) pages = 1;
    uint64_t base = g_binary_load_next;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            shell_write("loadbin: out of physical memory\n");
            kfree(tmp);
            return;
        }
        vmm_map(base + i * VMM_PAGE_SIZE, frame, VMM_PRESENT | VMM_WRITABLE);
    }
    g_binary_load_next = base + pages * VMM_PAGE_SIZE;

    memcpy((void *)(uintptr_t)base, tmp, size);
    kfree(tmp);

    shell_printf("loadbin: loaded %s (%u bytes, %lu page(s))\n", argv[1], size, (unsigned long)pages);
    shell_printf("  virtual  base: 0x%lx\n", (unsigned long)base);
    shell_printf("  physical base: 0x%lx", (unsigned long)vmm_get_phys(base));
    if (pages > 1) {
        shell_printf(" (physical pages not guaranteed contiguous beyond the first --\n"
                     "   a real DMA driver would need to check each page individually)");
    }
    shell_write("\n");
}

static void tasks_run(void) {
    while (1) {
        console_clear();
        console_printf("ApexOS Task Monitor -- Esc to exit, auto-refreshes every second\n\n");
        console_printf("  PID  STATE    NAME\n");
        console_printf("    0  running  shell\n");
        int n = scheduler_task_count();
        for (int i = 0; i < n; i++) {
            console_printf("  %3d  %s  %s\n", i + 1,
                           scheduler_task_is_done(i) ? "done   " : "ready  ", scheduler_task_name(i));
        }
        console_printf("\nscheduler: preemption %s (%d kernel task(s) besides the shell)\n",
                       scheduler_preemption_enabled() ? "ON" : "off", n);
        console_printf("uptime : %lu s\n", (unsigned long)(pit_get_ticks() / 100));
        console_printf("memory : %lu / %lu KiB free/total (physical)\n",
                       (unsigned long)(pmm_free_frames() * PMM_FRAME_SIZE / 1024),
                       (unsigned long)(pmm_total_frames() * PMM_FRAME_SIZE / 1024));
        console_printf("kheap  : %lu KiB used, %lu KiB free\n",
                       (unsigned long)(kheap_used_bytes() / 1024),
                       (unsigned long)(kheap_free_bytes() / 1024));
        console_printf("\n(honest: only kernel-mode threads are scheduled here -- a ring3\n"
                       " program started with `run`/Ctrl+R still takes over the whole CPU\n"
                       " until it exits; see `help sched`.)\n");
        serial_write("[tasks] view refreshed\n");

        uint64_t deadline = pit_get_ticks() + 100; /* обновление раз в секунду */
        int exited = 0;
        while (pit_get_ticks() < deadline) {
            int ev = keyboard_read_key();
            if (ev == 27) { exited = 1; break; } /* Esc */
            __asm__ volatile ("hlt");
        }
        if (exited) {
            console_clear();
            return;
        }
    }
}

static void cmd_dmesg(int argc, char **argv) {
    (void)argc; (void)argv;
    size_t avail = serial_log_available();
    char *buf = (char *)kmalloc(avail > 0 ? avail : 1);
    if (buf == NULL) { shell_write("dmesg: out of memory\n"); return; }
    serial_log_copy(buf, avail);
    console_write_len(buf, avail);
    /* Не дублируем в serial: этот же текст УЖЕ прошёл через serial
       (это и есть источник лога), повторная печать туда была бы
       бессмысленным эхом. Только на экран. */
    kfree(buf);
}

static void cmd_tasks(int argc, char **argv) {
    (void)argc; (void)argv;
    tasks_run();
}

static void cmd_lspci(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("bus:sl.f  vendor      device      class info\n");
    pci_enumerate(lspci_callback, NULL);
}

static void cmd_uname(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("ApexOS 0.1 x86_64 (custom kernel, not Linux-compatible)\n");
}

static void cmd_free(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_printf("physical: %lu KiB total, %lu KiB free\n",
                 (unsigned long)(pmm_total_frames() * PMM_FRAME_SIZE / 1024),
                 (unsigned long)(pmm_free_frames() * PMM_FRAME_SIZE / 1024));
    shell_printf("kheap:    %lu KiB used, %lu KiB free\n",
                 (unsigned long)(kheap_used_bytes() / 1024),
                 (unsigned long)(kheap_free_bytes() / 1024));
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_printf("ramdisk (FAT32): %lu clusters total, %lu free (~%lu KiB free)\n",
                 (unsigned long)fat32_total_clusters(), (unsigned long)fat32_free_clusters(),
                 (unsigned long)(fat32_free_clusters() * fat32_cluster_size_bytes() / 1024));
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("  PID  STATE    NAME\n");
    shell_write("    0  running  shell\n");
    int n = scheduler_task_count();
    for (int i = 0; i < n; i++) {
        shell_printf("  %3d  %s  %s\n", i + 1,
                     scheduler_task_is_done(i) ? "done   " : "ready  ", scheduler_task_name(i));
    }
    if (n == 0) {
        shell_write("(no kernel tasks -- see `spawn` and `sched`)\n");
    }
}

static int g_demo_counter_a = 0;
static int g_demo_counter_b = 0;

static void demo_task_a(void) {
    for (int i = 0; i < 5; i++) {
        g_demo_counter_a++;
        serial_printf("[demo-a] tick %d\n", g_demo_counter_a);
        uint64_t target = pit_get_ticks() + 50;
        while (pit_get_ticks() < target) {
            task_yield(); /* добровольно отдаём CPU, а не просто крутим busy-loop -- честная кооперативность
                              вдобавок к вытесняющему переключению по таймеру */
        }
    }
}

static void demo_task_b(void) {
    for (int i = 0; i < 5; i++) {
        g_demo_counter_b++;
        serial_printf("[demo-b] tick %d\n", g_demo_counter_b);
        uint64_t target = pit_get_ticks() + 70;
        while (pit_get_ticks() < target) {
            task_yield();
        }
    }
}

static void cmd_spawn(int argc, char **argv) {
    (void)argc; (void)argv;
    g_demo_counter_a = 0;
    g_demo_counter_b = 0;
    task_create("demo-a", demo_task_a);
    task_create("demo-b", demo_task_b);
    shell_write("spawn: created demo-a and demo-b -- watch `dmesg`/serial for interleaved\n"
                "[demo-a]/[demo-b] tick lines proving they actually alternate execution.\n"
                "Run `sched on` if they don't seem to progress (preemption starts off).\n");
}

static void cmd_sched(int argc, char **argv) {
    if (argc < 2) {
        shell_printf("sched: preemption is currently %s\n",
                     scheduler_preemption_enabled() ? "ON" : "off");
        shell_write("sched: usage: sched on|off\n");
        return;
    }
    if (strcmp(argv[1], "on") == 0) {
        scheduler_set_preemption(1);
        shell_write("sched: preemption ON -- kernel tasks now get timer-driven time slices\n");
    } else if (strcmp(argv[1], "off") == 0) {
        scheduler_set_preemption(0);
        shell_write("sched: preemption off -- kernel tasks only run via their own task_yield()\n");
    } else {
        shell_write("sched: usage: sched on|off\n");
    }
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("reboot: pulsing the 8042 keyboard controller reset line...\n");
    uint8_t status;
    int guard = 0;
    do {
        status = inb(0x64);
        if (guard++ > 100000) break;
    } while (status & 0x02);
    outb(0x64, 0xFE);
    shell_write("reboot: no response from the controller, halting instead\n");
    halt_forever();
}

static void cmd_halt(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("System halted. It is now safe to power off.\n");
    halt_forever();
}

static void cmd_run(int argc, char **argv) {
    if (argc < 2) { shell_write("run: missing ELF64 file name\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("run: invalid name\n"); return; }

    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("run: out of memory\n"); return; }

    uint32_t real_size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE, &real_size) != 0) {
        shell_write("run: file not found\n");
        kfree(buf);
        return;
    }
    if (real_size > FILE_BUF_SIZE) {
        shell_printf("run: file too large (%u bytes, limit %u) -- not running it\n",
                     real_size, (unsigned)FILE_BUF_SIZE);
        kfree(buf);
        return;
    }

    uint64_t entry = 0, user_stack = 0;
    if (elf64_load(buf, real_size, &entry, &user_stack) != 0) {
        shell_write("run: ELF64 load failed (see [elf64] log above)\n");
        kfree(buf);
        return;
    }
    kfree(buf); /* elf64_load already copied the segments to their final addresses */

    shell_printf("run: entering ring3 at 0x%lx...\n", (unsigned long)entry);
    int exit_code = process_run(entry, user_stack);
    shell_printf("run: program exited with code %d -- back in the shell\n", exit_code);
}

struct shell_command {
    const char *name;
    void (*fn)(int argc, char **argv);
};

static void cmd_sh(int argc, char **argv); /* определена ниже, после execute_line, которую она использует */

static const struct shell_command COMMANDS[] = {
    { "help",    cmd_help },
    { "clear",   cmd_clear },
    { "echo",    cmd_echo },
    { "pwd",     cmd_pwd },
    { "ls",      cmd_ls },
    { "cd",      cmd_cd },
    { "mkdir",   cmd_mkdir },
    { "rmdir",   cmd_rmdir },
    { "touch",   cmd_touch },
    { "rm",      cmd_rm },
    { "cp",      cmd_cp },
    { "mv",      cmd_mv },
    { "cat",     cmd_cat },
    { "wc",      cmd_wc },
    { "head",    cmd_head },
    { "tail",    cmd_tail },
    { "grep",    cmd_grep },
    { "hexdump", cmd_hexdump },
    { "sleep",   cmd_sleep },
    { "history", cmd_history },
    { "nano",    cmd_nano },
    { "whoami",  cmd_whoami },
    { "users",   cmd_users },
    { "useradd", cmd_useradd },
    { "su",      cmd_su },
    { "sudo",    cmd_sudo },
    { "uname",   cmd_uname },
    { "date",    cmd_date },
    { "lspci",   cmd_lspci },
    { "loadbin", cmd_loadbin },
    { "free",    cmd_free },
    { "df",      cmd_df },
    { "ps",      cmd_ps },
    { "spawn",   cmd_spawn },
    { "sched",   cmd_sched },
    { "tasks",   cmd_tasks },
    { "dmesg",   cmd_dmesg },
    { "sh",      cmd_sh },
    { "run",     cmd_run },
    { "reboot",  cmd_reboot },
    { "halt",    cmd_halt },
};
#define NUM_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

/* --- переменные окружения shell'а: VAR=value присваивание, $VAR подстановка --- */
#define MAX_VARS 32
#define VAR_NAME_LEN 32
#define VAR_VALUE_LEN 192

struct shell_var {
    char name[VAR_NAME_LEN];
    char value[VAR_VALUE_LEN];
};
static struct shell_var g_vars[MAX_VARS];
static int g_var_count = 0;

static const char *var_get(const char *name) {
    for (int i = 0; i < g_var_count; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            return g_vars[i].value;
        }
    }
    return ""; /* необъявленная переменная -- пустая строка, как в настоящем shell */
}

static void var_set(const char *name, const char *value) {
    for (int i = 0; i < g_var_count; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            strcpy_safe(g_vars[i].value, sizeof(g_vars[i].value), value);
            return;
        }
    }
    if (g_var_count < MAX_VARS) {
        strcpy_safe(g_vars[g_var_count].name, sizeof(g_vars[g_var_count].name), name);
        strcpy_safe(g_vars[g_var_count].value, sizeof(g_vars[g_var_count].value), value);
        g_var_count++;
    }
}

/* Распознаёт "NAME=value" (без пробелов до '='), где NAME начинается
   с буквы/'_' и дальше состоит из букв/цифр/'_' -- если это НЕ
   присваивание (обычная команда с аргументами вида "cmd a=b"), сразу
   возвращает 0, ничего не портя в line. */
static int try_parse_assignment(char *line, char **out_name, char **out_value) {
    char *eq = NULL;
    for (char *p = line; *p; p++) {
        if (*p == ' ') return 0;
        if (*p == '=') { eq = p; break; }
    }
    if (eq == NULL || eq == line) return 0;
    for (char *p = line; p < eq; p++) {
        char c = *p;
        int is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        int is_digit_c = (c >= '0' && c <= '9');
        if (!(is_alpha || (p != line && is_digit_c))) {
            return 0;
        }
    }
    *eq = '\0';
    *out_name = line;
    *out_value = eq + 1;
    return 1;
}

/* execute_line: разбирает и исполняет ОДНУ строку -- общий путь для
   интерактивного приглашения и скриптов (`sh`), чтобы не дублировать
   логику присваивания/подстановки/диспетчеризации в двух местах. */
static void execute_line(char *line) {
    char *assign_name, *assign_value;
    if (try_parse_assignment(line, &assign_name, &assign_value)) {
        var_set(assign_name, assign_value);
        return;
    }

    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv, MAX_ARGS);
    if (argc == 0) {
        return;
    }

    /* $VAR-подстановка -- отдельный буфer, т.к. значение переменной
       может быть длиннее исходного токена "$X". */
    static char expanded_storage[MAX_ARGS][MAX_LINE];
    char *expanded_argv[MAX_ARGS];
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '$' && argv[i][1] != '\0') {
            strcpy_safe(expanded_storage[i], sizeof(expanded_storage[i]), var_get(argv[i] + 1));
        } else {
            strcpy_safe(expanded_storage[i], sizeof(expanded_storage[i]), argv[i]);
        }
        expanded_argv[i] = expanded_storage[i];
    }

    int found = 0;
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(expanded_argv[0], COMMANDS[i].name) == 0) {
            COMMANDS[i].fn(argc, expanded_argv);
            found = 1;
            break;
        }
    }
    if (!found) {
        shell_printf("%s: command not found (see `help`)\n", expanded_argv[0]);
    }
}

#define MAX_SCRIPT_LINES 256

static int line_starts_with_word(const char *line, const char *word) {
    size_t wlen = strlen(word);
    if (strncmp(line, word, wlen) != 0) return 0;
    char next = line[wlen];
    return next == '\0' || next == ' ' || next == '\t';
}

static char *skip_leading_space(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* find_matching: ищет строку с первым словом close_word, соответствующую
   ТЕКУЩЕМУ open_word на строке start-1 -- считает только вложенность
   ОДНОИМЁННЫХ пар (if/fi считает только if/fi, не задевая вложенные
   for/done, и наоборот), поэтому произвольная вложенность if-внутри-for
   и наоборот работает корректно без путаницы между типами блоков. */
static int find_matching(char **lines, int start, int end, const char *open_word, const char *close_word) {
    int depth = 1;
    for (int i = start; i < end; i++) {
        char *t = skip_leading_space(lines[i]);
        if (line_starts_with_word(t, open_word)) {
            depth++;
        } else if (line_starts_with_word(t, close_word)) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

/* eval_test: "[ ... ]" -- честно ограниченный набор: -f/-d (файл/каталог
   существует), -z/-n (строка пуста/непуста), = / != (сравнение строк).
   t/n -- ровно токены МЕЖДУ "if" и "then" (включая сами '[' и ']'). */
static int eval_test(int n, char **t) {
    if (n < 2 || strcmp(t[0], "[") != 0 || strcmp(t[n - 1], "]") != 0) {
        shell_write("if: expected `[ ... ]` (ApexOS scripts don't support bare command exit codes)\n");
        return 0;
    }
    int m = n - 2;
    char **c = t + 1;
    if (m == 2 && strcmp(c[0], "-f") == 0) {
        char name83[FAT32_NAME_LEN];
        struct fat32_dirent e;
        if (fat32_name_to_83(c[1], name83) != 0) return 0;
        if (!fat32_find_entry(g_cwd_cluster, name83, &e)) return 0;
        return !(e.attr & FAT32_ATTR_DIRECTORY);
    }
    if (m == 2 && strcmp(c[0], "-d") == 0) {
        char name83[FAT32_NAME_LEN];
        struct fat32_dirent e;
        if (fat32_name_to_83(c[1], name83) != 0) return 0;
        if (!fat32_find_entry(g_cwd_cluster, name83, &e)) return 0;
        return (e.attr & FAT32_ATTR_DIRECTORY) != 0;
    }
    if (m == 2 && strcmp(c[0], "-z") == 0) return c[1][0] == '\0';
    if (m == 2 && strcmp(c[0], "-n") == 0) return c[1][0] != '\0';
    if (m == 3 && strcmp(c[1], "=") == 0) return strcmp(c[0], c[2]) == 0;
    if (m == 3 && strcmp(c[1], "!=") == 0) return strcmp(c[0], c[2]) != 0;
    shell_write("if: unsupported test expression\n");
    return 0;
}

static void exec_block(char **lines, int start, int end) {
    int i = start;
    while (i < end) {
        char *trimmed = skip_leading_space(lines[i]);
        if (*trimmed == '\0' || *trimmed == '#') { i++; continue; }

        char line_copy[MAX_LINE];
        strcpy_safe(line_copy, sizeof(line_copy), trimmed);
        char *argv[MAX_ARGS];
        int argc = tokenize(line_copy, argv, MAX_ARGS);

        if (argc >= 2 && strcmp(argv[0], "if") == 0 && strcmp(argv[argc - 1], "then") == 0) {
            int fi_line = find_matching(lines, i + 1, end, "if", "fi");
            if (fi_line < 0) { shell_write("sh: missing `fi`\n"); return; }
            if (eval_test(argc - 2, argv + 1)) {
                exec_block(lines, i + 1, fi_line);
            }
            i = fi_line + 1;
            continue;
        }

        if (argc >= 4 && strcmp(argv[0], "for") == 0 && strcmp(argv[2], "in") == 0 &&
            strcmp(argv[argc - 1], "do") == 0) {
            int done_line = find_matching(lines, i + 1, end, "for", "done");
            if (done_line < 0) { shell_write("sh: missing `done`\n"); return; }
            char var_name[VAR_NAME_LEN];
            strcpy_safe(var_name, sizeof(var_name), argv[1]);
            int item_count = argc - 4;
            for (int k = 0; k < item_count; k++) {
                var_set(var_name, argv[3 + k]);
                exec_block(lines, i + 1, done_line);
            }
            i = done_line + 1;
            continue;
        }

        execute_line(trimmed);
        i++;
    }
}

static void cmd_sh(int argc, char **argv) {
    if (argc < 2) { shell_write("sh: usage: sh <script file>\n"); return; }
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(argv[1], name83) != 0) { shell_write("sh: invalid name\n"); return; }

    char *buf = (char *)kmalloc(FILE_BUF_SIZE);
    if (buf == NULL) { shell_write("sh: out of memory\n"); return; }
    uint32_t size = 0;
    if (fat32_read_file(g_cwd_cluster, name83, buf, FILE_BUF_SIZE - 1, &size) != 0) {
        shell_write("sh: script not found\n");
        kfree(buf);
        return;
    }
    if (size >= FILE_BUF_SIZE) size = FILE_BUF_SIZE - 1;
    buf[size] = '\0';

    static char *script_lines[MAX_SCRIPT_LINES];
    int line_count = 0;
    char *line_start = buf;
    for (uint32_t i = 0; i <= size; i++) {
        if (i == size || buf[i] == '\n') {
            if (i == size && line_start == buf + i) break;
            buf[i] = '\0';
            if (line_count < MAX_SCRIPT_LINES) {
                script_lines[line_count++] = line_start;
            } else {
                shell_write("sh: script has too many lines, truncating\n");
                break;
            }
            line_start = buf + i + 1;
        }
    }
    exec_block(script_lines, 0, line_count);
    kfree(buf);
}

void fetch() {
    shell_write("     /\\     |\\ |¯¯¯¯    \\ /        |¯¯¯¯|  |¯¯¯   \n");
    shell_write("    /  \\    |/  |____     \\    ===  |    |  |__    \n");
    shell_write("   /----\\   |   |        / \\   ===  |    |     |   \n");
    shell_write("  /      \\  |   |____   /   \\       |____|  ___|   \n");
    
}

void shell_run(void) {
    g_cwd_cluster = fat32_root_cluster();
    g_path_depth = 0;

    shell_write("\nApexOS shell. Type `help` for a list of commands.\n");

    char line[MAX_LINE];

    while (1) {
        print_prompt();
        read_line(line, sizeof(line));
        history_add(line);
        execute_line(line);
    }
}
