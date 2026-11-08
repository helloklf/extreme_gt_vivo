/* sim_fccc_turbo.c - 在主机上把 fccc_turbo 的 syscall 层换成模拟文件系统,跑场景矩阵。
 *
 * 构建(WSL/Linux):
 *   gcc -DHOSTTEST -O1 -Wall -Wextra -Werror src/fccc_turbo/fccc_turbo.c \
 *       test/sim_fccc_turbo.c -o sim_fccc_turbo
 * 运行:
 *   ./sim_fccc_turbo
 *
 * 模拟内容:
 *   /sys/kernel/debug/pmic-votable/DCHG_FCC/force_val|force_active  votable 强制通道
 *   /sys/class/cms_class/{fixed_temp,main_batt_temp}                温度
 *   /sys/class/power_supply/battery/{voltage_now,capacity,current_now,status}
 *   /sys/class/power_supply/usb/{voltage_now,current_now}           充电器输出
 *
 * 验证:功率天花板换算(恒功率,电压升则电流降)、温度限流与退出、
 *       SOC>70 释放、未插电释放、SIGTERM 释放、外部改动检测(不还原)、
 *       启动清理残留 force、pwr=0 不强制、自定义功率。
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
static void (*g_on_tick)(int);
static void *g_sig15;
static char  g_writes[MAXW][64];
static int   g_nwrites;

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

static void record_write(const char *path, const void *buf, long len)
{
    const char *base = strrchr(path, '/');
    int n;
    if (g_nwrites >= MAXW)
        return;
    base = base ? base + 1 : path;
    n = snprintf(g_writes[g_nwrites], sizeof g_writes[g_nwrites], "%s=", base);
    if (n < 0)
        return;
    if (len > (long)(sizeof g_writes[g_nwrites]) - n - 1)
        len = (long)(sizeof g_writes[g_nwrites]) - n - 1;
    memcpy(g_writes[g_nwrites] + n, buf, (size_t)len);
    g_writes[g_nwrites][n + len] = 0;
    g_nwrites++;
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
        map_path(path, mp, sizeof mp);
        if (flags & 0100)           /* O_CREAT(锁文件) */
            f = fopen(mp, "a+");
        else if (flags & 1)         /* O_WRONLY:sysfs 语义,覆盖而非追加 */
            f = fopen(mp, "w");
        else
            f = fopen(mp, "r");
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
        if (a == 1) {
            fwrite((void *)b, 1, (size_t)c, stdout);
            return c;
        }
        if (a == 2) {               /* 日志直通 stderr,便于调试 */
            fwrite((void *)b, 1, (size_t)c, stderr);
            return c;
        }
        f = g_fd[a];
        if (!f)
            return -1;
        w = fwrite((void *)b, 1, (size_t)c, f);
        fflush(f);
        if (strstr(g_fdpath[a], "force_val") ||
            strstr(g_fdpath[a], "force_active"))
            record_write(g_fdpath[a], (void *)b, (long)c);
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
        if (ms >= 900) {
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
    case 32:  /* flock */
    case 40:  /* mount */
        return 0;
    default:
        return 0;
    }
}

/* ---------------- 场景辅助 ---------------- */

static void set_temp(int deci_c)
{
    char b[32];
    snprintf(b, sizeof b, "%d\n", deci_c);
    write_file("/sys/class/cms_class/main_batt_temp", b);
}

static void set_vbat(long uv)
{
    char b[32];
    snprintf(b, sizeof b, "%ld\n", uv);
    write_file("/sys/class/power_supply/battery/voltage_now", b);
}

static void set_cap(long pct)
{
    char b[32];
    snprintf(b, sizeof b, "%ld\n", pct);
    write_file("/sys/class/power_supply/battery/capacity", b);
}

static void set_status(const char *s)
{
    write_file("/sys/class/power_supply/battery/status", s);
}

static void set_force(long val, long active)
{
    char b[32];
    snprintf(b, sizeof b, "%ld", val);
    write_file("/sys/kernel/debug/pmic-votable/DCHG_FCC/force_val", b);
    snprintf(b, sizeof b, "%ld", active);
    write_file("/sys/kernel/debug/pmic-votable/DCHG_FCC/force_active", b);
}

/* 每个用例的默认现场:25C、4.30V、50%、8A 充电中、force 未激活、htccc 开启 */
static void setup_defaults(void)
{
    write_file("/sys/class/cms_class/fixed_temp", "0\n");
    write_file("/sys/class/cms_class/htccc_enable", "1\n");
    set_temp(250);
    set_vbat(4300000);
    set_cap(50);
    write_file("/sys/class/power_supply/battery/current_now", "-8000000\n");
    set_status("Charging\n");
    write_file("/sys/class/power_supply/usb/voltage_now", "9000000\n");
    write_file("/sys/class/power_supply/usb/current_now", "3000000\n");
    set_force(0, 0);
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

static int run_case(int limit, void (*tick)(int), const char *pwr_arg)
{
    char *argv[4] = { (char *)"fccc_turbo", NULL, NULL, NULL };
    int argc = 1, r;

    if (pwr_arg) {
        argv[argc++] = (char *)pwr_arg;
    }
    setup_defaults();
    reset_fds();
    g_ticks = 0;
    g_limit = limit;
    g_on_tick = tick;
    g_nwrites = 0;
    g_sig15 = NULL;

    hosttest_reset();
    r = setjmp(g_env);
    if (r == 0)
        run(argc, argv);
    return r;
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

/* 用例结束后检查 htccc_enable 的落盘值 */
static int check_htccc(const char *name, const char *want)
{
    char mp[512], buf[16] = { 0 };
    FILE *f;
    int ok;
    map_path("/sys/class/cms_class/htccc_enable", mp, sizeof mp);
    f = fopen(mp, "r");
    if (!f)
        die("htccc read");
    if (!fread(buf, 1, sizeof buf - 1, f))
        buf[0] = 0;
    fclose(f);
    ok = strncmp(buf, want, strlen(want)) == 0;
    printf("%-24s %s  htccc_enable=%s", name, ok ? "PASS" : "FAIL", buf);
    if (!ok)
        printf(" (want %s)", want);
    printf("\n");
    return ok ? 0 : 1;
}

/* 与 fccc_turbo.c 相同的定点温度曲线(用于精确断言),基数为功率换算电流 */
static long scaled_r(long current, int ticks, int percent)
{
    long factor = 1000000000000L;
    while (ticks-- > 0) factor = factor * percent / 100;
    return current * factor / 1000000000000L;
}

static long temp_ma_replica(long temp, long prev_temp, long base)
{
    long current = base, ct;
    if (temp < 470 && prev_temp >= 0 && temp < prev_temp) return base;
    if (temp <= 461) return base;
    ct = temp > 480 ? 480 : temp;
    current = scaled_r(current, ct > 461 ? (int)(ct - 461) : 0, 94);
    if (ct > 470) current = scaled_r(current, (int)(ct - 470), 85);
    return current < base / 3 ? 0 : current;
}

/* ---------------- tick 回调 ---------------- */

static void tick_vbat_rise(int n)
{
    if (n == 1) set_vbat(4450000);
    if (n == 2) set_vbat(4500000);
}

static void tick_cap75(int n)
{
    if (n == 1) set_cap(75);
}

static void tick_cool_down(int n)
{
    if (n == 1) set_temp(250);
}

static void tick_external_change(int n)
{
    if (n == 1) {
        set_force(5000, 1);     /* 外部改写 force_val */
        set_status("Discharging\n");
    }
}

static void tick_sigterm(int n)
{
    if (n == 1 && g_sig15)
        ((void (*)(int))g_sig15)(15);
}

static void tick_unplug(int n)
{
    if (n == 1) set_status("Discharging\n");
}

static void tick_temp_465(int n)
{
    if (n == 1) set_temp(465);      /* 468 -> 465:区间内降温 */
}

static void tick_temp_471(int n)
{
    if (n == 1) set_temp(471);      /* 472 -> 471:高温区降温 */
}

int main(void)
{
    volatile int fail = 0;

    snprintf(g_root, sizeof g_root, "/tmp/fccc-turbo-sim-%d", (int)getpid());
    mkdirs(g_root);
    {
        char p[512];
        map_path("/data/local/tmp", p, sizeof p);
        mkdirs(p);
        map_path("/sys/class/cms_class", p, sizeof p);
        mkdirs(p);
        map_path("/sys/class/power_supply/battery", p, sizeof p);
        mkdirs(p);
        map_path("/sys/class/power_supply/usb", p, sizeof p);
        mkdirs(p);
        map_path("/sys/kernel/debug/pmic-votable/DCHG_FCC", p, sizeof p);
        mkdirs(p);
    }

    /* 1) 功率天花板:40W @ 4.30V -> 9302mA,只写一次;
     *    运行中 htccc 保持 0(接管中) */
    {
        const char *exp[] = { "force_val=9302", "force_active=1" };
        int rc = run_case(3, NULL, "40");
        fail += expect("pwr40 ceiling", exp, 2, rc, 1000);
        fail += check_htccc("htccc disabled", "0");
    }

    /* 2) 恒功率随电压收敛:4.30V 9302 -> 4.45V 8988 -> 4.50V 电压棘轮接管 7960 */
    {
        const char *exp[] = { "force_val=9302", "force_active=1",
                              "force_val=8988", "force_active=1",
                              "force_val=7960", "force_active=1" };
        int rc = run_case(3, tick_vbat_rise, "40");
        fail += expect("voltage taper", exp, 6, rc, 1000);
    }

    /* 3) SOC > 70:释放 force */
    {
        const char *exp[] = { "force_val=9302", "force_active=1",
                              "force_active=0", "force_val=0" };
        int rc = run_case(3, tick_cap75, "40");
        fail += expect("capacity release", exp, 4, rc, 1000);
    }

    /* 4) 温度限流(47.2C)后温度回落 -> 回到功率天花板 */
    {
        char tbuf[32];
        const char *exp[4];
        int rc;
        snprintf(tbuf, sizeof tbuf, "force_val=%ld", temp_ma_replica(472, -1, 9302));
        exp[0] = tbuf;
        exp[1] = "force_active=1";
        exp[2] = "force_val=9302";
        exp[3] = "force_active=1";
        setup_defaults();
        set_temp(472);          /* setup_defaults 之后前置,避免被重置 */
        reset_fds();
        g_ticks = 0;
        g_limit = 2;
        g_on_tick = tick_cool_down;
        g_nwrites = 0;
        g_sig15 = NULL;
        hosttest_reset();
        {
            char *argv[] = { (char *)"fccc_turbo", (char *)"40", NULL };
            int r = setjmp(g_env);
            if (r == 0)
                run(2, argv);
            rc = r;
        }
        fail += expect("temp limit+exit", exp, 4, rc, 1000);
    }

    /* 5) 外部改写 force_val:不还原,报错退出(rc=101);
     *    即使出错退出,htccc 也必须恢复 1 */
    {
        const char *exp[] = { "force_val=9302", "force_active=1" };
        int rc = run_case(5, tick_external_change, "40");
        fail += expect("external change", exp, 2, rc, 101);
        fail += check_htccc("htccc restored(err)", "1");
    }

    /* 6) SIGTERM:释放后退出(rc=100),htccc 恢复 1 */
    {
        const char *exp[] = { "force_val=9302", "force_active=1",
                              "force_active=0", "force_val=0" };
        int rc = run_case(10, tick_sigterm, "40");
        fail += expect("sigterm cleanup", exp, 4, rc, 100);
        fail += check_htccc("htccc restored", "1");
    }

    /* 7) 未插电:释放 */
    {
        const char *exp[] = { "force_val=9302", "force_active=1",
                              "force_active=0", "force_val=0" };
        int rc = run_case(3, tick_unplug, "40");
        fail += expect("unplug release", exp, 4, rc, 1000);
    }

    /* 8) pwr=0:不限速,不强制任何电流 */
    {
        int rc = run_case(2, NULL, "0");
        fail += expect("pwr0 no force", NULL, 0, rc, 1000);
    }

    /* 9) pwr=20:20W @ 4.30V -> 4651mA */
    {
        const char *exp[] = { "force_val=4651", "force_active=1" };
        int rc = run_case(2, NULL, "20");
        fail += expect("pwr20 arg", exp, 2, rc, 1000);
    }

    /* 10) 启动清理残留 force(上次崩溃留下的 7000mA);
     *     同时验证默认 60W:4.30V 下 13953mA < BASE,功率墙生效 */
    {
        const char *exp[] = { "force_active=0", "force_val=0",
                              "force_val=13953", "force_active=1" };
        int rc;
        setup_defaults();
        set_force(7000, 1);
        reset_fds();
        g_ticks = 0;
        g_limit = 2;
        g_on_tick = NULL;
        g_nwrites = 0;
        g_sig15 = NULL;
        hosttest_reset();
        {
            char *argv[] = { (char *)"fccc_turbo", NULL };
            int r = setjmp(g_env);
            if (r == 0)
                run(1, argv);
            rc = r;
        }
        fail += expect("stale force clear", exp, 4, rc, 1000);
    }

    /* 11) 默认 60W + 温度限流(47.2C):温度曲线(5103)比功率墙(13953)更低,温度接管 */
    {
        char tbuf[32];
        const char *exp[2];
        int rc;
        snprintf(tbuf, sizeof tbuf, "force_val=%ld", temp_ma_replica(472, -1, 13953));
        exp[0] = tbuf;
        exp[1] = "force_active=1";
        setup_defaults();
        set_temp(472);
        reset_fds();
        g_ticks = 0;
        g_limit = 2;
        g_on_tick = NULL;
        g_nwrites = 0;
        g_sig15 = NULL;
        hosttest_reset();
        {
            char *argv[] = { (char *)"fccc_turbo", NULL };
            int r = setjmp(g_env);
            if (r == 0)
                run(1, argv);
            rc = r;
        }
        fail += expect("default 60W+temp", exp, 2, rc, 1000);
    }

    /* 12) cap_rel=100:SOC 75 也不交还,继续强制(突破高电量渐降) */
    {
        const char *exp[] = { "force_val=13953", "force_active=1" };
        int rc;
        setup_defaults();
        set_cap(75);
        reset_fds();
        g_ticks = 0;
        g_limit = 2;
        g_on_tick = NULL;
        g_nwrites = 0;
        g_sig15 = NULL;
        hosttest_reset();
        {
            char *argv[] = { (char *)"fccc_turbo", (char *)"60",
                             (char *)"100", NULL };
            int r = setjmp(g_env);
            if (r == 0)
                run(3, argv);
            rc = r;
        }
        fail += expect("cap_rel100 keep", exp, 2, rc, 1000);
    }

    /* 13) MAX_FCC 钳制:120W @ 3.00V = 40000mA -> 17600 */
    {
        const char *exp[] = { "force_val=17600", "force_active=1" };
        int rc;
        setup_defaults();
        set_vbat(3000000);
        reset_fds();
        g_ticks = 0;
        g_limit = 2;
        g_on_tick = NULL;
        g_nwrites = 0;
        g_sig15 = NULL;
        hosttest_reset();
        {
            char *argv[] = { (char *)"fccc_turbo", (char *)"120", NULL };
            int r = setjmp(g_env);
            if (r == 0)
                run(2, argv);
            rc = r;
        }
        fail += expect("max_fcc clamp", exp, 2, rc, 1000);
    }

    /* 14) 降温趋势豁免:46.8C(限流) -> 46.5C(区间内降温) -> 不降速,回 base */
    {
        char t1[32];
        const char *exp[4];
        int rc;
        snprintf(t1, sizeof t1, "force_val=%ld", temp_ma_replica(468, -1, 9302));
        exp[0] = t1;
        exp[1] = "force_active=1";
        exp[2] = "force_val=9302";
        exp[3] = "force_active=1";
        setup_defaults();
        set_temp(468);
        reset_fds();
        g_ticks = 0;
        g_limit = 2;
        g_on_tick = tick_temp_465;
        g_nwrites = 0;
        g_sig15 = NULL;
        hosttest_reset();
        {
            char *argv[] = { (char *)"fccc_turbo", (char *)"40", NULL };
            int r = setjmp(g_env);
            if (r == 0)
                run(2, argv);
            rc = r;
        }
        fail += expect("cooling exempt", exp, 4, rc, 1000);
    }

    /* 15) 豁免只限第一档区间:47.2C -> 47.1C(仍 > 47.0)继续降速 */
    {
        char t1[32], t2[32];
        const char *exp[4];
        int rc;
        snprintf(t1, sizeof t1, "force_val=%ld", temp_ma_replica(472, -1, 9302));
        snprintf(t2, sizeof t2, "force_val=%ld", temp_ma_replica(471, 472, 9302));
        exp[0] = t1;
        exp[1] = "force_active=1";
        exp[2] = t2;
        exp[3] = "force_active=1";
        setup_defaults();
        set_temp(472);
        reset_fds();
        g_ticks = 0;
        g_limit = 2;
        g_on_tick = tick_temp_471;
        g_nwrites = 0;
        g_sig15 = NULL;
        hosttest_reset();
        {
            char *argv[] = { (char *)"fccc_turbo", (char *)"40", NULL };
            int r = setjmp(g_env);
            if (r == 0)
                run(2, argv);
            rc = r;
        }
        fail += expect("hot-zone no exempt", exp, 4, rc, 1000);
    }

    printf("\n%s (%d failure%s)\n", fail ? "FAILED" : "ALL PASS", (int)fail,
           fail == 1 ? "" : "s");
    return fail ? 1 : 0;
}
