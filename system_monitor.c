/*
 * system_monitor.c  --  System Health Monitor
 * CEP146 NDD Lab 9
 *
 * Build: gcc -Wall -O2 -o system_monitor system_monitor.c -lncurses
 * Run:   ./system_monitor
 *
 * Features:
 *   - Real-time CPU utilization (total + per core) with bar graphs
 *   - Real-time memory usage with breakdown by type, bar graph
 *   - Process killer: type a PID and press Enter to send SIGKILL
 *   - Email alerts when CPU or memory exceed configured thresholds
 *     (configure in alert_config.txt)
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

/* ── tunables ─────────────────────────────────────────────────────────── */
#define MAX_CORES   64
#define BAR_WIDTH   40
#define ALERT_FILE  "alert_config.txt"

/* ncurses colour-pair IDs */
#define CP_GREEN    1
#define CP_YELLOW   2
#define CP_RED      3
#define CP_TITLE    4
#define CP_LABEL    5
#define CP_INPUT    6

/* ── data types ───────────────────────────────────────────────────────── */
typedef struct {
    unsigned long long user, nice, system, idle,
                       iowait, irq, softirq, steal;
} CPUStat;

typedef struct {
    long long total, free, available, buffers, cached, slab, used;
} MemInfo;

/* ── globals ──────────────────────────────────────────────────────────── */
static CPUStat  g_prev[MAX_CORES + 1]; /* [0] = aggregate, [1..n] = cores */
static CPUStat  g_curr[MAX_CORES + 1];
static double   g_cpu_pct[MAX_CORES + 1];
static int      g_num_cores = 0;
static MemInfo  g_mem;

static char     g_pid_buf[16]      = "";
static char     g_status[256]      = "";
static char     g_alert_email[256] = "";
static double   g_cpu_thresh       = 80.0;
static double   g_mem_thresh       = 80.0;
static int      g_alert_cpu_sent   = 0;
static int      g_alert_mem_sent   = 0;

/* ── alert config ─────────────────────────────────────────────────────── */
static void load_alert_config(void)
{
    FILE *f = fopen(ALERT_FILE, "r");
    if (!f) {
        snprintf(g_status, sizeof g_status,
                 "No %s found – alerts disabled.", ALERT_FILE);
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if      (strncmp(line, "email=",          6 ) == 0) {
            snprintf(g_alert_email, sizeof g_alert_email, "%.255s", line + 6);
        } else if (strncmp(line, "cpu_threshold=", 14) == 0) {
            g_cpu_thresh = atof(line + 14);
        } else if (strncmp(line, "mem_threshold=", 14) == 0) {
            g_mem_thresh = atof(line + 14);
        }
    }
    fclose(f);
}

static void send_alert(const char *subject, const char *body)
{
    if (!g_alert_email[0]) return;
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "echo '%s' | mail -s '%s' '%s' 2>/dev/null",
             body, subject, g_alert_email);
    int _r = system(cmd); (void)_r;
}

static void check_alerts(void)
{
    double mem_pct = (g_mem.total > 0)
        ? 100.0 * (g_mem.total - g_mem.available) / g_mem.total : 0.0;

    /* CPU alert */
    if (g_cpu_pct[0] >= g_cpu_thresh && !g_alert_cpu_sent) {
        char body[256];
        snprintf(body, sizeof body,
                 "CPU usage %.1f%% exceeds threshold %.1f%%",
                 g_cpu_pct[0], g_cpu_thresh);
        send_alert("System Monitor: CPU Alert", body);
        g_alert_cpu_sent = 1;
        snprintf(g_status, sizeof g_status,
                 "CPU alert sent to %.180s", g_alert_email);
    } else if (g_cpu_pct[0] < g_cpu_thresh) {
        g_alert_cpu_sent = 0;
    }

    /* Memory alert */
    if (mem_pct >= g_mem_thresh && !g_alert_mem_sent) {
        char body[256];
        snprintf(body, sizeof body,
                 "Memory usage %.1f%% exceeds threshold %.1f%%",
                 mem_pct, g_mem_thresh);
        send_alert("System Monitor: Memory Alert", body);
        g_alert_mem_sent = 1;
        snprintf(g_status, sizeof g_status,
                 "Memory alert sent to %.177s", g_alert_email);
    } else if (mem_pct < g_mem_thresh) {
        g_alert_mem_sent = 0;
    }
}

/* ── /proc readers ────────────────────────────────────────────────────── */
static int read_cpu_stats(CPUStat stats[], int *ncores)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[256];
    *ncores = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        char name[16];
        CPUStat s = {0, 0, 0, 0, 0, 0, 0, 0};
        sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
               name,
               &s.user, &s.nice, &s.system, &s.idle,
               &s.iowait, &s.irq, &s.softirq, &s.steal);
        if (strcmp(name, "cpu") == 0) {
            stats[0] = s;
        } else {
            int core = atoi(name + 3);
            if (core >= 0 && core < MAX_CORES) {
                stats[core + 1] = s;
                if (core + 1 > *ncores) *ncores = core + 1;
            }
        }
    }
    fclose(f);
    return 0;
}

static double calc_cpu_pct(const CPUStat *prev, const CPUStat *curr)
{
    unsigned long long pi = prev->idle  + prev->iowait;
    unsigned long long ci = curr->idle  + curr->iowait;
    unsigned long long pn = prev->user  + prev->nice  + prev->system
                          + prev->irq   + prev->softirq + prev->steal;
    unsigned long long cn = curr->user  + curr->nice  + curr->system
                          + curr->irq   + curr->softirq + curr->steal;
    unsigned long long dtotal = (cn + ci) - (pn + pi);
    unsigned long long didle  = ci - pi;
    if (dtotal == 0) return 0.0;
    return 100.0 * (double)(dtotal - didle) / (double)dtotal;
}

static int read_mem_info(MemInfo *m)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128], key[64], unit[8];
    long long val;
    memset(m, 0, sizeof *m);
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63s %lld %7s", key, &val, unit) >= 2) {
            if      (!strcmp(key, "MemTotal:"))     m->total     = val;
            else if (!strcmp(key, "MemFree:"))      m->free      = val;
            else if (!strcmp(key, "MemAvailable:")) m->available = val;
            else if (!strcmp(key, "Buffers:"))      m->buffers   = val;
            else if (!strcmp(key, "Cached:"))       m->cached    = val;
            else if (!strcmp(key, "Slab:"))         m->slab      = val;
        }
    }
    fclose(f);
    m->used = m->total - m->free - m->buffers - m->cached - m->slab;
    if (m->used < 0) m->used = 0;
    return 0;
}

/* ── drawing helpers ──────────────────────────────────────────────────── */
static void draw_bar(int y, int x, double pct, int width)
{
    int filled = (int)(pct / 100.0 * width + 0.5);
    if (filled > width) filled = width;
    if (filled < 0)     filled = 0;

    int pair = (pct < 50.0) ? CP_GREEN
             : (pct < 80.0) ? CP_YELLOW : CP_RED;

    mvaddch(y, x, '[');
    attron(COLOR_PAIR(pair) | A_BOLD);
    for (int i = 0; i < filled; i++) addch(ACS_BLOCK);
    attroff(COLOR_PAIR(pair) | A_BOLD);
    for (int i = filled; i < width; i++) addch('-');
    addch(']');
}

/* ── main display ─────────────────────────────────────────────────────── */
static void draw_screen(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int bar_w = (cols > 70) ? BAR_WIDTH : (cols - 22);
    if (bar_w < 8) bar_w = 8;

    clear();
    int row = 0;

    /* ── Title bar ── */
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD | A_REVERSE);
    char title[] = "  System Health Monitor  ";
    mvprintw(row, (cols - (int)strlen(title)) / 2, "%s", title);
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD | A_REVERSE);
    row++;

    /* timestamp */
    time_t now = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, " %Y-%m-%d  %H:%M:%S", localtime(&now));
    mvprintw(row, 0, "%s", tbuf);
    row += 2;

    /* ── CPU total ── */
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(row, 0, "CPU Usage (Total)");
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    row++;
    draw_bar(row, 2, g_cpu_pct[0], bar_w);
    mvprintw(row, bar_w + 5, "%5.1f%%", g_cpu_pct[0]);
    row += 2;

    /* ── CPU per core ── */
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(row, 0, "CPU Per Core");
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    row++;
    for (int i = 0; i < g_num_cores && row < rows - 14; i++) {
        mvprintw(row, 2, "Core %2d:", i);
        draw_bar(row, 11, g_cpu_pct[i + 1], bar_w);
        mvprintw(row, bar_w + 13, "%5.1f%%", g_cpu_pct[i + 1]);
        row++;
    }
    row++;

    /* ── Memory ── */
    double mem_used_pct = (g_mem.total > 0)
        ? 100.0 * (g_mem.total - g_mem.available) / g_mem.total : 0.0;

    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(row, 0, "Memory Usage   (Total: %lld MB)",
             g_mem.total / 1024LL);
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    row++;
    draw_bar(row, 2, mem_used_pct, bar_w);
    mvprintw(row, bar_w + 5, "%5.1f%%", mem_used_pct);
    row += 2;

    /* breakdown */
    long long bufcache_kb = g_mem.buffers + g_mem.cached;
    double proc_pct  = (g_mem.total > 0)
        ? 100.0 * g_mem.used      / g_mem.total : 0.0;
    double bc_pct    = (g_mem.total > 0)
        ? 100.0 * bufcache_kb     / g_mem.total : 0.0;
    double kern_pct  = (g_mem.total > 0)
        ? 100.0 * g_mem.slab      / g_mem.total : 0.0;

    int sb = bar_w - 14;
    if (sb < 6) sb = 6;

    mvprintw(row, 2, "Processes:");
    draw_bar(row, 14, proc_pct, sb);
    mvprintw(row, sb + 17, "%6lld MB (%4.1f%%)",
             g_mem.used / 1024LL, proc_pct);
    row++;

    mvprintw(row, 2, "Buf/Cache:");
    draw_bar(row, 14, bc_pct, sb);
    mvprintw(row, sb + 17, "%6lld MB (%4.1f%%)",
             bufcache_kb / 1024LL, bc_pct);
    row++;

    mvprintw(row, 2, "Kernel:   ");
    draw_bar(row, 14, kern_pct, sb);
    mvprintw(row, sb + 17, "%6lld MB (%4.1f%%)",
             g_mem.slab / 1024LL, kern_pct);
    row++;

    mvprintw(row, 2, "Free:     %6lld MB", g_mem.free / 1024LL);
    row += 2;

    /* ── Process Killer ── */
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(row, 0, "Process Killer");
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    row++;
    mvprintw(row, 2, "PID to kill: ");
    attron(COLOR_PAIR(CP_INPUT) | A_BOLD);
    mvprintw(row, 15, "%-12s", g_pid_buf);
    attroff(COLOR_PAIR(CP_INPUT) | A_BOLD);
    mvprintw(row, 28, " (type digits, Enter to send SIGKILL)");
    row += 2;

    /* ── Status ── */
    if (g_status[0]) {
        attron(COLOR_PAIR(CP_RED) | A_BOLD);
        mvprintw(row, 0, "Status: %s", g_status);
        attroff(COLOR_PAIR(CP_RED) | A_BOLD);
        row++;
    }

    /* alert config summary */
    if (row < rows - 1) {
        if (g_alert_email[0])
            mvprintw(row, 0,
                     "Alerts: CPU>%.0f%% or Mem>%.0f%% -> %s",
                     g_cpu_thresh, g_mem_thresh, g_alert_email);
        else
            mvprintw(row, 0,
                     "Alerts: disabled  (create %s to enable)", ALERT_FILE);
    }

    /* footer */
    attron(A_REVERSE);
    mvprintw(rows - 1, 0,
             " Q: Quit  |  Type a PID above + Enter to kill it ");
    attroff(A_REVERSE);

    refresh();
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void)
{
    /* ── ncurses init ── */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_GREEN,  COLOR_GREEN,  -1);
        init_pair(CP_YELLOW, COLOR_YELLOW, -1);
        init_pair(CP_RED,    COLOR_RED,    -1);
        init_pair(CP_TITLE,  COLOR_CYAN,   -1);
        init_pair(CP_LABEL,  COLOR_CYAN,   -1);
        init_pair(CP_INPUT,  COLOR_WHITE,  COLOR_BLUE);
    }

    load_alert_config();

    /* prime first CPU sample */
    read_cpu_stats(g_prev, &g_num_cores);
    sleep(1);

    /* ── main loop ── */
    for (;;) {
        /* sample */
        read_cpu_stats(g_curr, &g_num_cores);
        for (int i = 0; i <= g_num_cores; i++)
            g_cpu_pct[i] = calc_cpu_pct(&g_prev[i], &g_curr[i]);
        memcpy(g_prev, g_curr, sizeof g_curr);
        read_mem_info(&g_mem);
        check_alerts();
        draw_screen();

        /* poll input for ~1 second in 50 ms slices */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };
        for (int ticks = 20; ticks > 0; ticks--) {
            int ch = getch();
            if (ch == ERR) {
                nanosleep(&ts, NULL);
                continue;
            }
            if (ch == 'q' || ch == 'Q') {
                endwin();
                return 0;
            } else if (ch == '\n' || ch == KEY_ENTER) {
                if (g_pid_buf[0]) {
                    pid_t pid = (pid_t)atoi(g_pid_buf);
                    if (pid > 1) {
                        if (kill(pid, SIGKILL) == 0)
                            snprintf(g_status, sizeof g_status,
                                     "PID %d killed.", pid);
                        else
                            snprintf(g_status, sizeof g_status,
                                     "kill(%d) failed: %s",
                                     pid, strerror(errno));
                    } else {
                        snprintf(g_status, sizeof g_status,
                                 "Invalid PID: %s", g_pid_buf);
                    }
                    g_pid_buf[0] = '\0';
                    draw_screen();
                }
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                int len = (int)strlen(g_pid_buf);
                if (len > 0) g_pid_buf[len - 1] = '\0';
                draw_screen();
            } else if (isdigit(ch) && strlen(g_pid_buf) < 10) {
                int len = (int)strlen(g_pid_buf);
                g_pid_buf[len]     = (char)ch;
                g_pid_buf[len + 1] = '\0';
                draw_screen();
            }
            nanosleep(&ts, NULL);
        }
    }

    endwin();
    return 0;
}
