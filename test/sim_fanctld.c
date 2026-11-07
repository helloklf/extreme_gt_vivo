/* sim_fanctld.c - 在主机上把 fanctld 的 syscall 层换成模拟文件系统,跑场景矩阵。
 *
 * 构建(WSL/Linux):
 *   gcc -DHOSTTEST -O1 -Wall -Wextra -Werror src/fanctld/fanctld.c test/sim_fanctld.c -o sim_fanctld
 * 运行:
 *   ./sim_fanctld
 *
 * 模拟内容:
 *   /dev/fanpwm                          普通文件,记录每次写入
 *   /sys/class/thermal/thermal_zone47    type=ddr, temp 可改
 *   /sys/class/cms_class/main_batt_temp  电池温度(0.1C)
 *   /data/local/tmp/fanctld.log          日志
 *
 * 验证:启动归零、起转脉冲、故障重试、曲线插值(status)、双路取最大、
 *       最小占空比、死区、降速限幅、缺 zone 退化、SIGTERM 归零和坏配置。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern void run(long argc, char **argv);
extern void hosttest_reset(void);

#define MAXFD 64
#define MAXW  64

static FILE *g_fd[MAXFD];
static char  g_fdpath[MAXFD][192];
static jmp_buf g_env;
static char  g_root[256];
static int   g_ticks, g_limit;
static int   g_fail_fan_opens, g_fail_fan_write_at, g_fan_write_attempts;
static int   g_test_fail_fan_opens, g_test_fail_fan_write_at;
static void (*g_on_tick)(int);
static void *g_sig15;
static char  g_writes[MAXW][16];
static int   g_nwrites;
static char  g_stdout[1024];
static int   g_outlen;

static void die(const char *m)
{
    fprintf(stderr, "harness: %s\n", m);
    exit(2);
}

static void map_path(const char *in, char *out, size_t n)
{
    if (in[0] == '/')
        snprintf(out, n, "%s%s", g_root, in);
    else
        snprintf(out, n, "%s/%s", g_root, in);
}

static void mkdirs(const char *path)
{
    char tmp[512];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const char *data)
{
    char mp[512], dir[512], *s;
    FILE *f;
    map_path(path, mp, sizeof mp);
    snprintf(dir, sizeof dir, "%s", mp);
    s = strrchr(dir, '/');
    if (s) {
        *s = 0;
        mkdirs(dir);
    }
    f = fopen(mp, "w");
    if (!f)
        die("write_file");
    fputs(data, f);
    fclose(f);
}

long sim_sc(long n, long a, long b, long c, long d)
{
    (void)d;
    switch (n) {
    case 56: {  /* openat */
        const char *path = (const char *)b;
        int flags = (int)c;
        char mp[512];
        int i;
        FILE *f;
        if (strstr(path, "/dev/fanpwm") && g_fail_fan_opens > 0) {
            g_fail_fan_opens--;
            return -1;
        }
        map_path(path, mp, sizeof mp);
        f = fopen(mp, (flags & 1) ? "a+" : "r");
        if (!f)
            return -1;
        for (i = 3; i < MAXFD; i++) {
            if (!g_fd[i]) {
                g_fd[i] = f;
                snprintf(g_fdpath[i], sizeof g_fdpath[i], "%s", path);
                return i;
            }
        }
        fclose(f);
        return -1;
    }
    case 63: {  /* read */
        FILE *f = g_fd[a];
        if (!f)
            return -1;
        return (long)fread((void *)b, 1, (size_t)c, f);
    }
    case 64: {  /* write */
        FILE *f;
        size_t w;
        if (a == 1) {           /* stdout,用于 status */
            fwrite((void *)b, 1, (size_t)c, stdout);
            if (g_outlen + (int)c < (int)sizeof g_stdout) {
                memcpy(g_stdout + g_outlen, (void *)b, (size_t)c);
                g_outlen += (int)c;
                g_stdout[g_outlen] = 0;
            }
            return c;
        }
        f = g_fd[a];
        if (!f)
            return -1;
        if (strstr(g_fdpath[a], "/dev/fanpwm")) {
            g_fan_write_attempts++;
            if (g_fan_write_attempts == g_fail_fan_write_at)
                return -1;
        }
        w = fwrite((void *)b, 1, (size_t)c, f);
        fflush(f);
        if (strstr(g_fdpath[a], "/dev/fanpwm") && g_nwrites < MAXW) {
            int len = (int)c < 15 ? (int)c : 15;
            memcpy(g_writes[g_nwrites], (void *)b, (size_t)len);
            g_writes[g_nwrites][len] = 0;
            g_nwrites++;
        }
        return (long)w;
    }
    case 57:  /* close */
        if (g_fd[a]) {
            fclose(g_fd[a]);
            g_fd[a] = NULL;
            g_fdpath[a][0] = 0;
        }
        return 0;
    case 101: {  /* nanosleep */
        struct timespec { long s, ns; } *t = (void *)a;
        long ms = t->s * 1000 + t->ns / 1000000;
        if (ms >= 900) {        /* 只统计常规轮询,忽略起转脉冲的 800ms */
            g_ticks++;
            if (g_on_tick)
                g_on_tick(g_ticks);
            if (g_ticks >= g_limit)
                longjmp(g_env, 1000);
        }
        return 0;
    }
    case 93:  /* exit */
        longjmp(g_env, 100 + (int)a);
    case 134:  /* rt_sigaction */
        if ((int)a == 15)
            g_sig15 = *(void **)b;
        return 0;
    default:
        return 0;
    }
}

/* ---------------- 场景辅助 ---------------- */

static void set_zone(const char *type, int temp_mc)
{
    char b[32];
    write_file("/sys/class/thermal/thermal_zone47/type", type);
    snprintf(b, sizeof b, "%d\n", temp_mc);
    write_file("/sys/class/thermal/thermal_zone47/temp", b);
}

static void set_batt(int deci_c)
{
    char b[32];
    snprintf(b, sizeof b, "%d\n", deci_c);
    write_file("/sys/class/cms_class/main_batt_temp", b);
}

static void reset_fds(void)
{
    int i;
    for (i = 0; i < MAXFD; i++) {
        if (g_fd[i]) {
            fclose(g_fd[i]);
            g_fd[i] = NULL;
        }
    }
}

static int run_case(const char *conf_text, int limit, void (*tick)(int))
{
    char mp[512];
    FILE *f;
    char *argv[] = { (char *)"fanctld", (char *)"/conf/fan.conf", NULL };
    int r;

    write_file("/conf/fan.conf", conf_text);
    map_path("/dev/fanpwm", mp, sizeof mp);
    f = fopen(mp, "w");
    if (!f)
        die("fanpwm");
    fclose(f);

    reset_fds();
    g_ticks = 0;
    g_limit = limit;
    g_on_tick = tick;
    g_fail_fan_opens = g_test_fail_fan_opens;
    g_fail_fan_write_at = g_test_fail_fan_write_at;
    g_fan_write_attempts = 0;
    g_test_fail_fan_opens = 0;
    g_test_fail_fan_write_at = 0;
    g_nwrites = 0;
    g_sig15 = NULL;
    g_outlen = 0;
    g_stdout[0] = 0;

    hosttest_reset();
    r = setjmp(g_env);
    if (r == 0)
        run(2, argv);
    return r;
}

static int do_status(const char *conf_text)
{
    char mp[512];
    FILE *f;
    char *argv[] = { (char *)"fanctld", (char *)"status",
                     (char *)"/conf/fan.conf", NULL };

    write_file("/conf/fan.conf", conf_text);
    map_path("/dev/fanpwm", mp, sizeof mp);
    f = fopen(mp, "w");
    if (f)
        fclose(f);
    reset_fds();
    g_outlen = 0;
    g_stdout[0] = 0;
    hosttest_reset();
    {
        int r = setjmp(g_env);
        if (r == 0)
            run(3, argv);
        return r;
    }
}

static int expect(const char *name, const char *exp[], int n, int rc, int want_rc)
{
    int ok = (rc == want_rc) && (g_nwrites == n);
    int i;

    for (i = 0; ok && i < n; i++)
        if (strcmp(g_writes[i], exp[i]))
            ok = 0;

    printf("%-24s %s  rc=%-4d writes=[", name, ok ? "PASS" : "FAIL", rc);
    for (i = 0; i < g_nwrites; i++)
        printf("%s%s", i ? "," : "", g_writes[i]);
    printf("]");
    if (!ok) {
        printf("\n    expected rc=%d writes=[", want_rc);
        for (i = 0; i < n; i++)
            printf("%s%s", i ? "," : "", exp[i]);
        printf("]");
    }
    printf("\n");
    return ok ? 0 : 1;
}

static int check_status(const char *name, int cond)
{
    printf("%-24s %s  out=%s", name, cond ? "PASS" : "FAIL", g_stdout);
    return cond ? 0 : 1;
}

static void tick_after(int n, int at, int temp_mc)
{
    if (n == at)
        set_zone("ddr\n", temp_mc);
}

static void tick_ramp_down(int n)
{
    tick_after(n, 1, 70000);      /* 80C -> 70C */
}

static void tick_ramp_stop(int n)
{
    tick_after(n, 1, 50000);      /* 60C -> 50C */
}

static void tick_deadband(int n)
{
    if (n == 1)
        set_zone("ddr\n", 94900);   /* 94.9C -> 99%,与 100% 差 1 < 死区 5 */
    if (n == 2)
        set_zone("ddr\n", 94800);   /* 94.8C -> 99% */
}

static void tick_sigterm(int n)
{
    if (n == 1 && g_sig15)
        ((void (*)(int))g_sig15)(15);
}

static void tick_restore_config(int n)
{
    if (n == 1)
        write_file("/conf/fan.conf",
                   "pop=95:100 90:80 80:70 70:50 60:30 50:0\nbatt=45:0\n");
}

int main(void)
{
    volatile int fail = 0;
    const char *conf_pop =
        "pop=95:100 90:80 80:70 70:50 60:30 50:0\n"
        "batt=45:0\n";

    snprintf(g_root, sizeof g_root, "/tmp/fanctld-sim-%d", (int)getpid());
    mkdirs(g_root);
    {
        char p[512];
        map_path("/dev", p, sizeof p);
        mkdirs(p);
        map_path("/conf", p, sizeof p);
        mkdirs(p);
        map_path("/data/local/tmp", p, sizeof p);
        mkdirs(p);
        map_path("/sys/class/thermal/thermal_zone47", p, sizeof p);
        mkdirs(p);
        map_path("/sys/class/cms_class", p, sizeof p);
        mkdirs(p);
    }

    /* 1) 启动归零 + 起转脉冲 */
    set_zone("ddr\n", 95000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "100" };
        int rc = run_case(conf_pop, 1, NULL);
        fail += expect("startup+kick", exp, 3, rc, 1000);
    }

    /* 1a) 配置暂不可用:修复后守护进程继续启动 */
    set_zone("ddr\n", 95000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "100" };
        int rc = run_case("batt_src=/x\n", 3, tick_restore_config);
        fail += expect("config retry", exp, 3, rc, 1000);
    }

    /* 1b) 设备暂不可用:稍后重试并正常控制 */
    set_zone("ddr\n", 95000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "100" };
        g_test_fail_fan_opens = 1;
        int rc = run_case(conf_pop, 2, NULL);
        fail += expect("device open retry", exp, 3, rc, 1000);
    }

    /* 1c) 初始清零写入失败:重开设备后继续启动 */
    set_zone("ddr\n", 95000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "100" };
        g_test_fail_fan_write_at = 1;
        int rc = run_case(conf_pop, 2, NULL);
        fail += expect("initial write retry", exp, 3, rc, 1000);
    }

    /* 1d) 运行中写入失败:重开设备后继续控制 */
    set_zone("ddr\n", 95000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "0", "45", "100" };
        g_test_fail_fan_write_at = 3;
        int rc = run_case(conf_pop, 3, NULL);
        fail += expect("fan write retry", exp, 5, rc, 1000);
    }

    /* 2) 曲线插值(status 不经过限幅) */
    set_zone("ddr\n", 95000);
    set_batt(200);
    do_status(conf_pop);
    fail += check_status("status 95C -> 100", strstr(g_stdout, "duty=100") != NULL);
    set_zone("ddr\n", 92000);
    do_status(conf_pop);
    fail += check_status("status 92C -> 88", strstr(g_stdout, "duty=88") != NULL);
    set_zone("ddr\n", 90000);
    do_status(conf_pop);
    fail += check_status("status 90C -> 80", strstr(g_stdout, "duty=80") != NULL);
    set_zone("ddr\n", 85000);
    do_status(conf_pop);
    fail += check_status("status 85C -> 75", strstr(g_stdout, "duty=75") != NULL);

    /* 3) 双路取最大:pop 50%,batt 80% -> 80 */
    set_zone("ddr\n", 70000);
    set_batt(430);
    {
        const char *exp[] = { "0", "45", "80" };
        int rc = run_case(
            "pop=70:50\nbatt=45:100 40:50\n", 2, NULL);
        fail += expect("max(pop,batt)", exp, 3, rc, 1000);
    }

    /* 4) 最小占空比:55C -> 15% -> 抬到 22% */
    set_zone("ddr\n", 55000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "22" };
        int rc = run_case("pop=60:30 50:0\nbatt=45:0\n", 2, NULL);
        fail += expect("min_duty", exp, 3, rc, 1000);
    }

    /* 5) 死区:95 -> 94.9 -> 94.8 只写一次 100 */
    set_zone("ddr\n", 95000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "100" };
        int rc = run_case(conf_pop, 3, tick_deadband);
        fail += expect("deadband", exp, 3, rc, 1000);
    }

    /* 6) 降速限幅:80C(70%) -> 70C(50%),每轮最多降 5 */
    set_zone("ddr\n", 80000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "70", "65", "60", "55", "50" };
        int rc = run_case("pop=80:70 70:50 60:30 50:0\nbatt=45:0\n",
                          5, tick_ramp_down);
        fail += expect("ramp-down 5%/step", exp, 7, rc, 1000);
    }

    /* 7) 降速到停:60C(30%) -> 50C(0%),逐级降到 0 */
    set_zone("ddr\n", 60000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "30", "25", "20", "15", "10",
                              "5", "0" };
        int rc = run_case("pop=60:30 50:0\nbatt=45:0\n", 7, tick_ramp_stop);
        fail += expect("ramp-down to stop", exp, 9, rc, 1000);
    }

    /* 8) 没有 ddr zone:退化为只用电池曲线 */
    set_zone("notddr\n", 95000);
    set_batt(430);
    {
        const char *exp[] = { "0", "45", "80" };
        int rc = run_case("pop=95:100\nbatt=45:100 40:50\n", 2, NULL);
        fail += expect("no-zone fallback", exp, 3, rc, 1000);
    }

    /* 9) SIGTERM:写 0 后退出 */
    set_zone("ddr\n", 95000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "100", "0" };
        int rc = run_case(conf_pop, 10, tick_sigterm);
        fail += expect("sigterm cleanup", exp, 4, rc, 100);
    }

    /* 10) status 的坏配置仍返回错误 */
    set_zone("ddr\n", 50000);
    set_batt(200);
    {
        int rc = do_status("batt_src=/x\n");
        fail += check_status("bad config", rc == 101);
    }

    /* 11) 曲线乱序:95:100 90:80,92C -> 88 */
    set_zone("ddr\n", 92000);
    set_batt(200);
    {
        const char *exp[] = { "0", "45", "88" };
        int rc = run_case("pop=70:50 95:100 90:80\nbatt=45:0\n", 2, NULL);
        fail += expect("unsorted curve", exp, 3, rc, 1000);
    }

    /* 12) status:缺 zone 时 pop=-1,只用电池 */
    set_zone("notddr\n", 95000);
    set_batt(430);
    do_status("pop=95:100\nbatt=45:100 40:50\n");
    fail += check_status("status no-zone",
                         strstr(g_stdout, "pop=-1") &&
                         strstr(g_stdout, "duty=80"));

    printf("\n%s (%d failure%s)\n", fail ? "FAILED" : "ALL PASS", (int)fail,
           fail == 1 ? "" : "s");
    return fail ? 1 : 0;
}
