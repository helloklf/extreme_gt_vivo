/* fanctld (raw-syscall, no libc) - 独立风扇控制器
 *
 * 两个温度输入,两条独立曲线,取两者算出的占空比的最大值:
 *   pop  : 默认取 thermal_zone 里 type=ddr 的那一路 tsens(只有一个节点,
 *          实测与 CPU 结温接近、略低;DDR 位于封装堆叠 PoP 上,所以叫 pop
 *          而不是 cpu,避免误导。也省掉遍历几十个 cpu-* 传感器)
 *   batt : 默认 /sys/class/cms_class/main_batt_temp (0.1C),与 tpoff 同源
 *
 * 输出 /dev/fanpwm:0-100 占空比。驱动 open() 上电、release() 断电,
 * 所以句柄全程保持打开;从停转起步时先给一个 45% 的起转脉冲越过静摩擦。
 * 启动有延迟防抖:转速需求连续 5 秒才允许起转(防瞬时尖峰频繁启停),
 * 但 pop(ddr)温度 >= 70C 时立即起转,不做等待。
 *
 * 配置文件默认取可执行文件同目录下的 fan.conf:
 * 温度单位 ℃,转速 0-100%,按温度从高到低写,两点之间线性插值,
 * 高于第一点取第一点,低于最后一点取最后一点(最后一点给 0 即自动停转)。
 *
 * 用法:
 *   fanctld [config]          常驻运行
 *   fanctld status [config]   打印一次当前温度和占空比后退出
 */
typedef long s64;
typedef unsigned long u64;

#define SYS_mount         40
#define SYS_openat        56
#define SYS_close         57
#define SYS_read          63
#define SYS_write         64
#define SYS_exit          93
#define SYS_nanosleep     101
#define SYS_rt_sigaction  134
#define SYS_rt_sigreturn  139

#define FAN_DEV    "/dev/fanpwm"
#define LOG_PATH   "/data/local/tmp/fanctld.log"

#define SA_RESTORER 0x04000000
#define MAXP 16
#define KICK_DUTY 45   /* 停转起步的起转脉冲:实测 20% 才起转,45% 足够且不吵 */
#define DUTY_HYST 5    /* 占空比死区:变化小于该值不重写,避免曲线边界处每秒抖动 */
#define MIN_DUTY 22    /* 运行中的最小占空比:实测 20% 才起转,留 2% 余量 */
#define DOWN_STEP 5    /* 降速限幅:每轮最多降 5%,避免骤停 */
#define START_DELAY 5      /* 延迟启动:转速需求连续 5 秒才允许起转,防瞬时尖峰启停 */
#define START_FORCE_POP 700  /* pop >= 70.0C(0.1C 单位)立即起转,不等 5 秒 */
#define INTERVAL_MS 1000   /* 轮询间隔 */
#define RETRY_INTERVAL_MS 5000

struct curve {
    int n;
    int t[MAXP];   /* 0.1C */
    int d[MAXP];   /* 0-100 */
};

static struct curve pop_curve, batt_curve;
static const char pop_zone[] = "ddr";
static char batt_path[128] = "/sys/class/cms_class/main_batt_temp";
static char zone_temp_path[128];
static int  g_fanfd = -1;
static char conf_path[256];

#ifdef HOSTTEST
/* 主机模拟测试:用模拟文件系统替换 syscall,详见 test/sim_fanctld.c */
extern long sim_sc(long n, long a, long b, long c, long d);
#define sc(n, a, b, c, d) sim_sc((long)(n), (long)(a), (long)(b), (long)(c), (long)(d))
#else
static inline s64 sc(s64 n, s64 a, s64 b, s64 c, s64 d)
{
    register s64 x8 asm("x8") = n;
    register s64 x0 asm("x0") = a;
    register s64 x1 asm("x1") = b;
    register s64 x2 asm("x2") = c;
    register s64 x3 asm("x3") = d;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                 : "memory");
    return x0;
}
#endif

static long parse_long(const char *s)
{
    long v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static long rd_long(const char *path, long fallback)
{
    char buf[64];
    long fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    long n;
    if (fd < 0)
        return fallback;
    n = sc(SYS_read, fd, (s64)buf, 63, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0)
        return fallback;
    buf[n] = 0;
    return parse_long(buf);
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    e = s + str_len(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        *--e = 0;
    return s;
}

static char *put_int(char *p, long v)
{
    char tmp[24];
    int i = 0, j = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { tmp[i++] = '0' + (int)(v % 10); v /= 10; } while (v);
    while (i)
        p[j++] = tmp[--i];
    return p + j;
}

static void log_msg(const char *m)
{
    long fd = sc(SYS_openat, -100, (s64)LOG_PATH, 1 | 0100 | 02000, 0644);
    if (fd >= 0) {
        sc(SYS_write, fd, (s64)m, str_len(m), 0);
        sc(SYS_close, fd, 0, 0, 0);
    }
}

static void log_state(const char *tag, long pop, long batt, long duty)
{
    char buf[160], *p = buf;
    while (*tag) *p++ = *tag++;
    *p++ = ' ';
    *p++ = 'p'; *p++ = 'o'; *p++ = 'p'; *p++ = '=';
    p = put_int(p, pop);
    *p++ = ' '; *p++ = 'b'; *p++ = 'a'; *p++ = 't'; *p++ = 't'; *p++ = '=';
    p = put_int(p, batt);
    *p++ = ' '; *p++ = 'd'; *p++ = 'u'; *p++ = 't'; *p++ = 'y'; *p++ = '=';
    p = put_int(p, duty);
    *p++ = '\n';
    *p = 0;
    log_msg(buf);
}

static void sleep_ms(long ms)
{
    long ts[2];
    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000L;
    sc(SYS_nanosleep, (s64)ts, 0, 0, 0);
}

/* 构造 /sys/class/thermal/thermal_zoneN/xxx 并读取 type 匹配 */
static int build_zone_path(int idx, const char *suffix, char *out)
{
    const char *a = "/sys/class/thermal/thermal_zone";
    char *p = out;
    while (*a) *p++ = *a++;
    p = put_int(p, idx);
    while (*suffix) *p++ = *suffix++;
    *p = 0;
    return 0;
}

static int find_zone(void)
{
    char tpath[128], buf[64];
    int i;
    for (i = 0; i < 128; i++) {
        long fd, n;
        build_zone_path(i, "/type", tpath);
        fd = sc(SYS_openat, -100, (s64)tpath, 0, 0);
        if (fd < 0)
            continue;
        n = sc(SYS_read, fd, (s64)buf, 63, 0);
        sc(SYS_close, fd, 0, 0, 0);
        if (n <= 0)
            continue;
        buf[n] = 0;
        {
            char *t = trim(buf);
            if (str_eq(t, pop_zone)) {
                build_zone_path(i, "/temp", zone_temp_path);
                return 0;
            }
        }
    }
    return -1;
}

/* 读 pop 源(默认 ddr),返回 0.1C;-1 表示失败 */
static long read_pop(void)
{
    long v;
    if (!zone_temp_path[0])
        return -1;
    v = rd_long(zone_temp_path, -10000);
    if (v <= -10000 || v <= 0 || v >= 200000)
        return -1;
    return v / 100;   /* 毫摄氏度 -> 0.1C */
}

static int duty_at(const struct curve *c, long t)
{
    int i;
    if (c->n <= 0)
        return 0;
    if (t >= c->t[0])
        return c->d[0];
    if (t <= c->t[c->n - 1])
        return c->d[c->n - 1];
    for (i = 1; i < c->n; i++) {
        if (t >= c->t[i]) {
            long t0 = c->t[i - 1], d0 = c->d[i - 1];
            long t1 = c->t[i], d1 = c->d[i];
            if (t1 == t0)
                return (int)d1;
            return (int)(d0 + (d1 - d0) * (t - t0) / (t1 - t0));
        }
    }
    return c->d[c->n - 1];
}

static void sort_curve(struct curve *c)
{
    int i, j;
    for (i = 1; i < c->n; i++) {
        int tt = c->t[i], dd = c->d[i];
        j = i - 1;
        while (j >= 0 && c->t[j] < tt) {
            c->t[j + 1] = c->t[j];
            c->d[j + 1] = c->d[j];
            j--;
        }
        c->t[j + 1] = tt;
        c->d[j + 1] = dd;
    }
}

static void parse_curve(struct curve *c, char *v)
{
    c->n = 0;
    while (*v && c->n < MAXP) {
        long t, d;
        while (*v == ' ' || *v == '\t')
            v++;
        if (!*v)
            break;
        t = parse_long(v);
        while (*v && *v != ':' && *v != ' ' && *v != '\t')
            v++;
        if (*v != ':')
            break;
        v++;
        d = parse_long(v);
        while (*v && *v != ' ' && *v != '\t')
            v++;
        if (d < 0) d = 0;
        if (d > 100) d = 100;
        c->t[c->n] = (int)(t * 10);
        c->d[c->n] = (int)d;
        c->n++;
    }
    sort_curve(c);
}

static int load_conf(const char *path)
{
    const char *default_batt = "/sys/class/cms_class/main_batt_temp";
    char buf[4096];
    long fd, n;
    char *p;
    int i;

    pop_curve.n = 0;
    batt_curve.n = 0;
    for (i = 0; default_batt[i] && i < (int)sizeof(batt_path) - 1; i++)
        batt_path[i] = default_batt[i];
    batt_path[i] = 0;
    fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    if (fd < 0)
        return -1;
    n = sc(SYS_read, fd, (s64)buf, 4095, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0)
        return -1;
    buf[n] = 0;

    p = buf;
    while (*p) {
        char *line = p, *eq, *k, *v;
        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        line = trim(line);
        if (!*line || *line == '#')
            continue;
        eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (!*eq)
            continue;
        *eq = 0;
        k = trim(line);
        v = trim(eq + 1);

        if (str_eq(k, "pop")) {
            parse_curve(&pop_curve, v);
        } else if (str_eq(k, "batt")) {
            parse_curve(&batt_curve, v);
        } else if (str_eq(k, "batt_src")) {
            int i = 0;
            while (v[i] && i < (int)sizeof(batt_path) - 1) {
                batt_path[i] = v[i];
                i++;
            }
            batt_path[i] = 0;
        }
    }
    return (pop_curve.n || batt_curve.n) ? 0 : -1;
}

/* 默认配置:可执行文件同目录下的 fan.conf */
static void default_conf(char *out, const char *argv0)
{
    const char *s = argv0;
    const char *slash = 0;
    while (*s) {
        if (*s == '/')
            slash = s;
        s++;
    }
    if (!slash) {
        const char *d = "fan.conf";
        char *p = out;
        while (*d) *p++ = *d++;
        *p = 0;
        return;
    }
    {
        int n = (int)(slash - argv0);
        int i;
        for (i = 0; i < n; i++)
            out[i] = argv0[i];
        out[n] = 0;
        {
            char *p = out + n;
            const char *d = "/fan.conf";
            while (*d) *p++ = *d++;
            *p = 0;
        }
    }
}

static int fan_open(void)
{
    g_fanfd = (int)sc(SYS_openat, -100, (s64)FAN_DEV, 1, 0);
    return g_fanfd >= 0 ? 0 : -1;
}

static int fan_write(long d)
{
    char buf[8], *p = buf;
    if (g_fanfd < 0)
        return -1;
    p = put_int(p, d);
    if (sc(SYS_write, g_fanfd, (s64)buf, p - buf, 0) != (p - buf))
        return -1;
    return 0;
}

static void fan_close_zero(void)
{
    if (g_fanfd >= 0) {
        fan_write(0);
        sc(SYS_close, g_fanfd, 0, 0, 0);
        g_fanfd = -1;
    }
}

static void fan_disconnect(void)
{
    if (g_fanfd >= 0)
        sc(SYS_close, g_fanfd, 0, 0, 0);
    g_fanfd = -1;
}

static void on_exit(int sig)
{
    (void)sig;
    fan_close_zero();
    log_msg("fanctld: exit, fan off\n");
    sc(SYS_exit, 0, 0, 0, 0);
    __builtin_unreachable();
}

#ifdef HOSTTEST
static void sig_restorer(void) {}
#else
static void __attribute__((naked)) sig_restorer(void)
{
    asm volatile("mov x8, %0\nsvc #0" :: "i"(SYS_rt_sigreturn));
}
#endif

struct ksigaction { u64 handler, flags, restorer, mask; };

static void setsig(int sig, void *handler)
{
    struct ksigaction a = { (u64)handler, SA_RESTORER, (u64)sig_restorer, 0 };
    sc(SYS_rt_sigaction, sig, (s64)&a, 0, 8);
}

#ifdef HOSTTEST
/* 模拟测试用:清掉上一轮的全局状态 */
void hosttest_reset(void)
{
    const char *d = "/sys/class/cms_class/main_batt_temp";
    int i = 0;
    pop_curve.n = 0;
    batt_curve.n = 0;
    zone_temp_path[0] = 0;
    while (d[i] && i < (int)sizeof(batt_path) - 1) {
        batt_path[i] = d[i];
        i++;
    }
    batt_path[i] = 0;
}
#endif

void __attribute__((noreturn)) run(long argc, char **argv)
{
    int status_only = 0;
    int have_conf = 0;
    int last = -1;
    int demand_s = 0;
    int config_error_logged = 0;
    int device_error_logged = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (str_eq(argv[i], "status"))
            status_only = 1;
        else if (!have_conf) {
            int j = 0;
            while (argv[i][j] && j < (int)sizeof(conf_path) - 1) {
                conf_path[j] = argv[i][j];
                j++;
            }
            conf_path[j] = 0;
            have_conf = 1;
        }
    }
    if (!have_conf)
        default_conf(conf_path, argv[0]);

    setsig(15, on_exit);
    setsig(2, on_exit);

    if (load_conf(conf_path)) {
        if (status_only) {
            log_msg("fanctld: config load failed\n");
            sc(SYS_exit, 1, 0, 0, 0);
        }
        do {
            if (!config_error_logged) {
                log_msg("fanctld: config load failed, retrying\n");
                config_error_logged = 1;
            }
            sleep_ms(RETRY_INTERVAL_MS);
        } while (load_conf(conf_path));
    }
    if (config_error_logged)
        log_msg("fanctld: config loaded\n");

    if (find_zone()) {
        /* 没有匹配的 pop 源时仍可用电池曲线,只是 pop 一路按 0 处理 */
        zone_temp_path[0] = 0;
    }

    if (status_only) {
        char buf[160], *p = buf;
        long pop = read_pop();
        long batt = rd_long(batt_path, -10000);
        int dp = pop > 0 ? duty_at(&pop_curve, pop) : 0;
        int db = batt > -10000 ? duty_at(&batt_curve, batt) : 0;
        int d = dp > db ? dp : db;
        if (d > 0 && d < MIN_DUTY)
            d = MIN_DUTY;
        *p++ = 'p'; *p++ = 'o'; *p++ = 'p'; *p++ = '=';
        if (pop > 0) p = put_int(p, pop); else { *p++='-'; *p++='1'; }
        *p++ = ' '; *p++='b'; *p++='a'; *p++='t'; *p++='t'; *p++='=';
        if (batt > -10000) p = put_int(p, batt); else { *p++='-'; *p++='1'; }
        *p++ = ' '; *p++='d'; *p++='u'; *p++='t'; *p++='y'; *p++='=';
        p = put_int(p, d);
        *p++ = '\n';
        sc(SYS_write, 1, (s64)buf, p - buf, 0);
        sc(SYS_exit, 0, 0, 0, 0);
    }

    for (;;) {
        if (fan_open()) {
            if (!device_error_logged) {
                log_msg("fanctld: cannot open /dev/fanpwm, retrying\n");
                device_error_logged = 1;
            }
            sleep_ms(RETRY_INTERVAL_MS);
            continue;
        }
        if (fan_write(0)) {
            if (!device_error_logged) {
                log_msg("fanctld: initial fan write failed, retrying\n");
                device_error_logged = 1;
            }
            fan_disconnect();
            sleep_ms(RETRY_INTERVAL_MS);
            continue;
        }
        if (device_error_logged)
            log_msg("fanctld: fan device recovered\n");
        device_error_logged = 0;
        log_msg("fanctld: start\n");
        last = 0;
        break;
    }

    for (;;) {
        if (g_fanfd < 0) {
            if (fan_open()) {
                if (!device_error_logged) {
                    log_msg("fanctld: cannot reopen /dev/fanpwm, retrying\n");
                    device_error_logged = 1;
                }
                sleep_ms(RETRY_INTERVAL_MS);
                continue;
            }
            if (fan_write(0)) {
                if (!device_error_logged)
                    log_msg("fanctld: fan write failed after reopen, retrying\n");
                device_error_logged = 1;
                fan_disconnect();
                sleep_ms(RETRY_INTERVAL_MS);
                continue;
            }
            log_msg("fanctld: fan device recovered\n");
            device_error_logged = 0;
            last = 0;
        }

        long pop = read_pop();
        long batt = rd_long(batt_path, -10000);
        int dp = pop > 0 ? duty_at(&pop_curve, pop) : 0;
        int db = batt > -10000 ? duty_at(&batt_curve, batt) : 0;
        long d = dp > db ? dp : db;

        if (d > 0 && d < MIN_DUTY)
            d = MIN_DUTY;
        if (d > 100)
            d = 100;

        /* 延迟启动:只有转速需求(d>0)连续 START_DELAY 秒才允许起转,
         * 防止瞬时温度尖峰导致频繁启停;pop >= 70C 立即起转。
         * 已在运转(last>0)时不参与,停止后重新累计。 */
        if (last <= 0) {
            if (d > 0) {
                if (pop >= START_FORCE_POP)
                    demand_s = START_DELAY;
                else if (demand_s < START_DELAY)
                    demand_s++;
                if (demand_s < START_DELAY)
                    d = 0;   /* 需求未满 5 秒,本轮继续停转 */
            } else {
                demand_s = 0;
            }
        }

        if (last > 0 && d < last && last - d > DOWN_STEP)
            d = last - DOWN_STEP;   /* 降速限幅,避免一帧掉太多 */
        if (last > 0 && d > 0) {
            long diff = d - last;
            if (diff < 0)
                diff = -diff;
            if (diff < DUTY_HYST)
                d = last;   /* 死区内不改,避免曲线边界处每秒抖动 */
        }

        if (d != last) {
            if (last <= 0 && d > 0) {
                /* 停转起步先踢一下,越过静摩擦后回到目标值 */
                if (fan_write(KICK_DUTY)) {
                    if (!device_error_logged)
                        log_msg("fanctld: fan kick write failed, retrying\n");
                    device_error_logged = 1;
                    fan_disconnect();
                    sleep_ms(INTERVAL_MS);
                    continue;
                }
                sleep_ms(800);
            }
            if (fan_write(d)) {
                if (!device_error_logged)
                    log_msg("fanctld: fan write failed, retrying\n");
                device_error_logged = 1;
                fan_disconnect();
                sleep_ms(INTERVAL_MS);
                continue;
            }
            log_state("fanctld:", pop, batt, d);
            device_error_logged = 0;
            last = (int)d;
        }
        sleep_ms(INTERVAL_MS);
    }
}

#ifndef HOSTTEST
void __attribute__((naked)) _start(void)
{
    asm volatile("ldr x0, [sp]\n"
                 "add x1, sp, #8\n"
                 "b run");
}
#endif
