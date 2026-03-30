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
 *   - Top-processes table sorted by CPU usage (live)
 *   - System uptime and load averages in the header
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
#include <dirent.h>

/* ── tunables ─────────────────────────────────────────────────────────── */
#define MAX_CORES   64
#define BAR_WIDTH   40
#define ALERT_FILE  "alert_config.txt"
#define MAX_PROCS   1024
#define TOP_PROCS   10      /* rows shown in the top-processes table */

/* ncurses colour-pair IDs */
#define CP_GREEN    1
#define CP_YELLOW   2
#define CP_RED      3
#define CP_TITLE    4
#define CP_LABEL    5
#define CP_INPUT    6
#define CP_DIM      7
#define CP_HILITE   8

/* ── data types ───────────────────────────────────────────────────────── */
typedef struct {
    unsigned long long user, nice, system, idle,
                       iowait, irq, softirq, steal;
} CPUStat;

typedef struct {
    long long total, free, available, buffers, cached, slab, used;
} MemInfo;

typedef struct {
    int                pid;
    unsigned long long ticks;   /* utime + stime from /proc/[pid]/stat */
} ProcSample;

typedef struct {
    int    pid;
    char   name[32];
    double cpu_pct;
    long   mem_kb;     /* VmRSS in kB */
} ProcInfo;

/* ── globals ──────────────────────────────────────────────────────────── */
static CPUStat    g_prev[MAX_CORES + 1];
static CPUStat    g_curr[MAX_CORES + 1];
static double     g_cpu_pct[MAX_CORES + 1];
static int        g_num_cores     = 0;
static MemInfo    g_mem;

/* elapsed CPU ticks (for per-process normalisation) */
static unsigned long long g_cpu_ticks_delta = 0;

/* process sampling */
static ProcSample g_ps_prev[MAX_PROCS];
static ProcSample g_ps_curr[MAX_PROCS];
static int        g_ps_prev_n = 0;
static int        g_ps_curr_n = 0;
static ProcInfo   g_top[TOP_PROCS];
static int        g_top_n = 0;

/* system info */
static double  g_uptime_sec  = 0.0;
static double  g_load[3]     = {0.0, 0.0, 0.0};  /* 1, 5, 15 min */

/* UI state */
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
        if      (strncmp(line, "email=",          6 ) == 0)
            snprintf(g_alert_email, sizeof g_alert_email, "%.255s", line + 6);
        else if (strncmp(line, "cpu_threshold=", 14) == 0)
            g_cpu_thresh = atof(line + 14);
        else if (strncmp(line, "mem_threshold=", 14) == 0)
            g_mem_thresh = atof(line + 14);
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

    if (g_cpu_pct[0] >= g_cpu_thresh && !g_alert_cpu_sent) {
        char body[256];
        snprintf(body, sizeof body, "CPU usage %.1f%% exceeds threshold %.1f%%",
                 g_cpu_pct[0], g_cpu_thresh);
        send_alert("System Monitor: CPU Alert", body);
        g_alert_cpu_sent = 1;
        snprintf(g_status, sizeof g_status,
                 "CPU alert sent to %.180s", g_alert_email);
    } else if (g_cpu_pct[0] < g_cpu_thresh) {
        g_alert_cpu_sent = 0;
    }

    if (mem_pct >= g_mem_thresh && !g_alert_mem_sent) {
        char body[256];
        snprintf(body, sizeof body, "Memory usage %.1f%% exceeds threshold %.1f%%",
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
        CPUStat s = {0,0,0,0,0,0,0,0};
        sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
               name, &s.user, &s.nice, &s.system, &s.idle,
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

/* returns total ticks elapsed (used to normalise per-process CPU%) */
static double calc_cpu_pct_delta(const CPUStat *prev, const CPUStat *curr,
                                  unsigned long long *out_total_delta)
{
    unsigned long long pi = prev->idle  + prev->iowait;
    unsigned long long ci = curr->idle  + curr->iowait;
    unsigned long long pn = prev->user  + prev->nice  + prev->system
                          + prev->irq   + prev->softirq + prev->steal;
    unsigned long long cn = curr->user  + curr->nice  + curr->system
                          + curr->irq   + curr->softirq + curr->steal;
    unsigned long long dtotal = (cn + ci) - (pn + pi);
    unsigned long long didle  = ci - pi;
    if (out_total_delta) *out_total_delta = dtotal;
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

static void read_uptime_loadavg(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { int _u = fscanf(f, "%lf", &g_uptime_sec); (void)_u; fclose(f); }
    f = fopen("/proc/loadavg", "r");
    if (f) { int _l = fscanf(f, "%lf %lf %lf", &g_load[0], &g_load[1], &g_load[2]); (void)_l; fclose(f); }
}

/* ── per-process sampling ─────────────────────────────────────────────── */

/* Read utime+stime ticks from /proc/<pid>/stat.  Returns 0 on success. */
static int read_proc_ticks(int pid, unsigned long long *ticks)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[512];
    int ok = (fgets(buf, sizeof buf, f) != NULL);
    fclose(f);
    if (!ok) return -1;
    /* skip past closing ')' of comm field */
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    p += 2;
    unsigned long utime = 0, stime = 0;
    /* fields after state: ppid pgrp session tty_nr tty_pgrp flags
       minflt cminflt majflt cmajflt utime stime  (13 items) */
    if (sscanf(p, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &utime, &stime) != 2) return -1;
    *ticks = (unsigned long long)(utime + stime);
    return 0;
}

/* Read process name from /proc/<pid>/comm */
static void read_proc_name(int pid, char *name, int size)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(name, size, "[%d]", pid); return; }
    if (!fgets(name, size, f)) name[0] = '\0';
    name[strcspn(name, "\r\n")] = '\0';
    fclose(f);
}

/* Read VmRSS from /proc/<pid>/status, returns kB */
static long read_proc_mem(int pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128], key[32];
    long val = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %ld", key, &val) == 2 &&
            strcmp(key, "VmRSS:") == 0) break;
        val = 0;
    }
    fclose(f);
    return val;
}

/* Scan /proc for all numeric dirs and fill samples array */
static int sample_all_procs(ProcSample samples[], int max)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *ent;
    int n = 0;
    while ((ent = readdir(d)) != NULL && n < max) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        int pid = atoi(ent->d_name);
        unsigned long long ticks = 0;
        if (read_proc_ticks(pid, &ticks) == 0) {
            samples[n].pid   = pid;
            samples[n].ticks = ticks;
            n++;
        }
    }
    closedir(d);
    return n;
}

/* Find a pid in a sorted-by-insertion samples array (linear, small N) */
static unsigned long long find_prev_ticks(int pid)
{
    for (int i = 0; i < g_ps_prev_n; i++)
        if (g_ps_prev[i].pid == pid) return g_ps_prev[i].ticks;
    return 0;
}

/* comparison for qsort: descending cpu_pct */
static int cmp_proc_cpu(const void *a, const void *b)
{
    const ProcInfo *pa = (const ProcInfo *)a;
    const ProcInfo *pb = (const ProcInfo *)b;
    if (pa->cpu_pct > pb->cpu_pct) return -1;
    if (pa->cpu_pct < pb->cpu_pct) return  1;
    return 0;
}

/* Build g_top[] from current + previous samples */
static void update_top_procs(void)
{
    if (g_cpu_ticks_delta == 0) { g_top_n = 0; return; }

    /* Scratch space for all processes */
    static ProcInfo all[MAX_PROCS];
    int n = 0;

    for (int i = 0; i < g_ps_curr_n && n < MAX_PROCS; i++) {
        int pid = g_ps_curr[i].pid;
        unsigned long long prev = find_prev_ticks(pid);
        unsigned long long delta = g_ps_curr[i].ticks - prev;

        ProcInfo *pi = &all[n++];
        pi->pid     = pid;
        pi->cpu_pct = 100.0 * (double)delta / (double)g_cpu_ticks_delta;
        pi->mem_kb  = read_proc_mem(pid);
        read_proc_name(pid, pi->name, sizeof pi->name);
    }

    qsort(all, n, sizeof(ProcInfo), cmp_proc_cpu);

    g_top_n = (n < TOP_PROCS) ? n : TOP_PROCS;
    for (int i = 0; i < g_top_n; i++)
        g_top[i] = all[i];
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

/* Format seconds as  "3d 04h 22m 11s" */
static void fmt_uptime(double sec, char *buf, int size)
{
    long s  = (long)sec;
    int  d  = s / 86400; s %= 86400;
    int  h  = s / 3600;  s %= 3600;
    int  m  = s / 60;    s %= 60;
    if (d > 0)
        snprintf(buf, size, "%dd %02dh %02dm %02ds", d, h, m, (int)s);
    else
        snprintf(buf, size, "%02dh %02dm %02ds", h, m, (int)s);
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

    /* timestamp + uptime + load */
    time_t now = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", localtime(&now));
    char uptbuf[32];
    fmt_uptime(g_uptime_sec, uptbuf, sizeof uptbuf);
    mvprintw(row, 0, " %s  |  Up: %s  |  Load: %.2f  %.2f  %.2f",
             tbuf, uptbuf, g_load[0], g_load[1], g_load[2]);
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
    for (int i = 0; i < g_num_cores && row < rows - 20; i++) {
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
    mvprintw(row, 0, "Memory Usage   (Total: %lld MB)", g_mem.total / 1024LL);
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    row++;
    draw_bar(row, 2, mem_used_pct, bar_w);
    mvprintw(row, bar_w + 5, "%5.1f%%", mem_used_pct);
    row += 2;

    long long bufcache_kb = g_mem.buffers + g_mem.cached;
    double proc_pct = (g_mem.total > 0) ? 100.0 * g_mem.used      / g_mem.total : 0.0;
    double bc_pct   = (g_mem.total > 0) ? 100.0 * bufcache_kb     / g_mem.total : 0.0;
    double kern_pct = (g_mem.total > 0) ? 100.0 * g_mem.slab      / g_mem.total : 0.0;

    int sb = bar_w - 14;
    if (sb < 6) sb = 6;

    mvprintw(row, 2, "Processes:");
    draw_bar(row, 14, proc_pct, sb);
    mvprintw(row, sb + 17, "%6lld MB (%4.1f%%)", g_mem.used     / 1024LL, proc_pct);
    row++;
    mvprintw(row, 2, "Buf/Cache:");
    draw_bar(row, 14, bc_pct, sb);
    mvprintw(row, sb + 17, "%6lld MB (%4.1f%%)", bufcache_kb    / 1024LL, bc_pct);
    row++;
    mvprintw(row, 2, "Kernel:   ");
    draw_bar(row, 14, kern_pct, sb);
    mvprintw(row, sb + 17, "%6lld MB (%4.1f%%)", g_mem.slab     / 1024LL, kern_pct);
    row++;
    mvprintw(row, 2, "Free:     %6lld MB", g_mem.free / 1024LL);
    row += 2;

    /* ── Top Processes ── */
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(row, 0, "Top Processes  (sorted by CPU)");
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    row++;

    /* table header */
    attron(COLOR_PAIR(CP_DIM) | A_BOLD | A_UNDERLINE);
    mvprintw(row, 2, "%-7s  %-20s  %6s  %8s",
             "PID", "NAME", "CPU%", "MEM(MB)");
    attroff(COLOR_PAIR(CP_DIM) | A_BOLD | A_UNDERLINE);
    row++;

    for (int i = 0; i < g_top_n && row < rows - 6; i++) {
        ProcInfo *p = &g_top[i];

        /* highlight top CPU user */
        if (i == 0 && p->cpu_pct > 0.1)
            attron(COLOR_PAIR(CP_HILITE) | A_BOLD);

        mvprintw(row, 2, "%-7d  %-20.20s  %5.1f%%  %7.1f",
                 p->pid, p->name, p->cpu_pct,
                 (double)p->mem_kb / 1024.0);

        if (i == 0 && p->cpu_pct > 0.1)
            attroff(COLOR_PAIR(CP_HILITE) | A_BOLD);
        row++;
    }
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

    /* ── Status / alerts ── */
    if (g_status[0] && row < rows - 2) {
        attron(COLOR_PAIR(CP_RED) | A_BOLD);
        mvprintw(row, 0, "Status: %s", g_status);
        attroff(COLOR_PAIR(CP_RED) | A_BOLD);
        row++;
    }
    if (row < rows - 1) {
        if (g_alert_email[0])
            mvprintw(row, 0, "Alerts: CPU>%.0f%% or Mem>%.0f%% -> %s",
                     g_cpu_thresh, g_mem_thresh, g_alert_email);
        else
            mvprintw(row, 0, "Alerts: disabled  (create %s to enable)", ALERT_FILE);
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
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_GREEN,  COLOR_GREEN,   -1);
        init_pair(CP_YELLOW, COLOR_YELLOW,  -1);
        init_pair(CP_RED,    COLOR_RED,     -1);
        init_pair(CP_TITLE,  COLOR_CYAN,    -1);
        init_pair(CP_LABEL,  COLOR_CYAN,    -1);
        init_pair(CP_INPUT,  COLOR_WHITE,   COLOR_BLUE);
        init_pair(CP_DIM,    COLOR_WHITE,   -1);
        init_pair(CP_HILITE, COLOR_YELLOW,  -1);
    }

    load_alert_config();

    /* prime CPU and process samples */
    read_cpu_stats(g_prev, &g_num_cores);
    g_ps_prev_n = sample_all_procs(g_ps_prev, MAX_PROCS);
    sleep(1);

    for (;;) {
        /* ── sample ── */
        read_cpu_stats(g_curr, &g_num_cores);
        g_cpu_pct[0] = calc_cpu_pct_delta(&g_prev[0], &g_curr[0],
                                           &g_cpu_ticks_delta);
        for (int i = 1; i <= g_num_cores; i++)
            g_cpu_pct[i] = calc_cpu_pct_delta(&g_prev[i], &g_curr[i], NULL);
        memcpy(g_prev, g_curr, sizeof g_curr);

        read_mem_info(&g_mem);
        read_uptime_loadavg();

        /* sample processes, build top table, then roll prev <- curr */
        g_ps_curr_n = sample_all_procs(g_ps_curr, MAX_PROCS);
        update_top_procs();
        memcpy(g_ps_prev, g_ps_curr, g_ps_curr_n * sizeof(ProcSample));
        g_ps_prev_n = g_ps_curr_n;

        check_alerts();
        draw_screen();

        /* poll input for ~1 second */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };
        for (int ticks = 20; ticks > 0; ticks--) {
            int ch = getch();
            if (ch == ERR) { nanosleep(&ts, NULL); continue; }

            if (ch == 'q' || ch == 'Q') { endwin(); return 0; }

            if (ch == '\n' || ch == KEY_ENTER) {
                if (g_pid_buf[0]) {
                    pid_t pid = (pid_t)atoi(g_pid_buf);
                    if (pid > 1) {
                        if (kill(pid, SIGKILL) == 0)
                            snprintf(g_status, sizeof g_status,
                                     "PID %d killed.", pid);
                        else
                            snprintf(g_status, sizeof g_status,
                                     "kill(%d) failed: %s", pid, strerror(errno));
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
