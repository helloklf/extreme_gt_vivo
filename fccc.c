/* Additional CMS current limit based on temperature (0.1 C) and voltage (uV). */
typedef long s64;
typedef unsigned long u64;

#define SYS_flock        32
#define SYS_openat       56
#define SYS_close        57
#define SYS_read         63
#define SYS_write        64
#define SYS_exit         93
#define SYS_nanosleep    101
#define SYS_rt_sigaction 134
#define SYS_rt_sigreturn 139

#define FIXED_NODE  "/sys/class/cms_class/fixed_temp"
#define TEMP_NODE   "/sys/class/cms_class/main_batt_temp"
#define VOLT_NODE   "/sys/class/power_supply/battery/voltage_now"
#define CAP_NODE    "/sys/class/power_supply/battery/capacity"
// #define LIMIT_NODE  "/sys/class/cms_class/debug_chg_cur"
#define LIMIT_NODE  "/sys/class/vfcs_class/fchg_fcc_ma"
#define STATUS_NODE "/sys/class/power_supply/battery/status"
#define LOCK_PATH   "/data/local/tmp/fccc.lock"

#define BASE_MA    15600
#define RELEASE_MA 0 // debug_chg_cur 写 17600 ， fchg_fcc_ma 写 0
#define MIN_MA      5000
#define TEMP_L1      431
#define TEMP_L2      440
#define TEMP_L3      450
#define TEMP_EXIT    421
#define VOLT_CRIT   4490000
#define VOLT_HIGH   4400000
#define VOLT_LOW    4310000
#define VOLT_DEC_FAST   50
#define VOLT_DEC_SLOW   25
#define CAP_RELEASE      70
#define VOLT_MIN_MA    4000
#define SA_RESTORER 0x04000000

#ifdef HOSTTEST
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

static int length(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void log_msg(const char *message)
{
    sc(SYS_write, 2, (s64)message, length(message), 0);
}

static char *put_number(char *p, long value)
{
    char tmp[24];
    int n = 0;
    if (value < 0) { *p++ = '-'; value = -value; }
    do { tmp[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) *p++ = tmp[--n];
    return p;
}

static void log_limit(long temp, long voltage, long capacity, long temp_ma,
                      long volt_ma, int temp_limited, int volt_limited,
                      long current, long target, int write_needed)
{
    char buf[320], *p = buf;
    const char *s = "fccc: temp_0.1C=";
    while (*s) *p++ = *s++;
    p = put_number(p, temp);
    s = " voltage_uV=";
    while (*s) *p++ = *s++;
    p = put_number(p, voltage);
    s = " capacity_pct=";
    while (*s) *p++ = *s++;
    p = put_number(p, capacity);
    s = " temp_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, temp_ma);
    s = " volt_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, volt_ma);
    s = " temp_limited=";
    while (*s) *p++ = *s++;
    p = put_number(p, temp_limited);
    s = " volt_limited=";
    while (*s) *p++ = *s++;
    p = put_number(p, volt_limited);
    s = " current_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, current);
    s = " requested_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, target);
    s = " write_needed=";
    while (*s) *p++ = *s++;
    p = put_number(p, write_needed);
    *p++ = '\n';
    *p = 0;
    log_msg(buf);
}

static int read_text(const char *path, char *buf, int size)
{
    long fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    long n;
    if (fd < 0) return -1;
    n = sc(SYS_read, fd, (s64)buf, size - 1, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0 || n >= size - 1) return -1;
    buf[n] = 0;
    return 0;
}

static int read_number(const char *path, long *value)
{
    char buf[64], *p = buf;
    long v = 0;
    int negative = 0, digits = 0;
    if (read_text(path, buf, sizeof(buf))) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { negative = 1; p++; }
    while (*p >= '0' && *p <= '9') {
        if (v > 1000000) return -1;
        v = v * 10 + *p++ - '0';
        digits++;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!digits || *p) return -1;
    *value = negative ? -v : v;
    return 0;
}

static int write_number(const char *path, long v)
{
    char buf[24], reverse[24];
    int i = 0, n = 0;
    long fd, written, back;
    do { reverse[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) buf[i++] = reverse[--n];
    fd = sc(SYS_openat, -100, (s64)path, 1, 0);
    if (fd < 0) return -1;
    written = sc(SYS_write, fd, (s64)buf, i, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (written != i || read_number(path, &back)) return -1;
    v = 0;
    for (n = 0; n < i; n++) v = v * 10 + buf[n] - '0';
    return back == v ? 0 : -1;
}

static int same_status(const char *buf, const char *expected)
{
    while (*expected && *buf == *expected) { buf++; expected++; }
    return !*expected && (*buf == '\n' || *buf == '\r' || !*buf);
}

static int charging(void)
{
    char buf[64];
    if (read_text(STATUS_NODE, buf, sizeof(buf))) return -1;
    if (same_status(buf, "Charging")) return 1;
    if (same_status(buf, "Discharging") || same_status(buf, "Not charging") ||
        same_status(buf, "Full")) return 0;
    return -1;
}

/* Fixed point reproduces the two separately truncated stages of the reference. */
static long scaled(long current, int ticks, int percent)
{
    long factor = 1000000000000L;
    while (ticks-- > 0) factor = factor * percent / 100;
    return current * factor / 1000000000000L;
}

static long current_for_temp(long temp, int *limited)
{
    long current = BASE_MA, ct;
    if (temp <= TEMP_L1 && (!*limited || temp <= TEMP_EXIT)) {
        *limited = 0;
        return current;
    }
    *limited = 1;
    if (current > 15000) current = 15000;
    ct = temp > TEMP_L3 ? TEMP_L3 : temp;
    current = scaled(current, ct > TEMP_L1 ? (int)(ct - TEMP_L1) : 0, 94);
    if (ct > TEMP_L2) current = scaled(current, (int)(ct - TEMP_L2), 85);
    return current < MIN_MA ? 0 : current;
}

/* Ratchet: each poll above VOLT_HIGH lowers the cap (faster above VOLT_CRIT),
 * never back up. Below VOLT_LOW the cap resets; in between it holds. Terminal
 * voltage moves with the charge current itself, so allowing upward adjustment
 * here would close a positive feedback loop and oscillate. */
static long cap_for_voltage(long voltage, long cap, int *limited)
{
    if (voltage < VOLT_LOW) {
        *limited = 0;
        return BASE_MA;
    }
    if (voltage > VOLT_HIGH) {
        *limited = 1;
        cap -= voltage > VOLT_CRIT ? VOLT_DEC_FAST : VOLT_DEC_SLOW;
        if (cap < VOLT_MIN_MA) cap = VOLT_MIN_MA;
    }
    return cap;
}

static volatile int stop_requested;
static void on_signal(int sig) { (void)sig; stop_requested = 1; }

#ifdef HOSTTEST
static void sig_restorer(void) {}
#else
static void __attribute__((naked)) sig_restorer(void)
{
    asm volatile("mov x8, %0\nsvc #0" :: "i"(SYS_rt_sigreturn));
}
#endif

struct ksigaction { u64 handler, flags, restorer, mask; };

static int setsig(int sig)
{
    struct ksigaction action = {
        (u64)on_signal, SA_RESTORER, (u64)sig_restorer, 0
    };
    return sc(SYS_rt_sigaction, sig, (s64)&action, 0, 8) < 0 ? -1 : 0;
}

static void sleep_second(void)
{
    long ts[2] = { 1, 0 };
    sc(SYS_nanosleep, (s64)ts, 0, 0, 0);
}

/* Never change a value that another writer has changed. */
static int release_limit(long *last)
{
    long current, fixed;
    if (*last < 0) return 0;
    if (read_number(FIXED_NODE, &fixed) || fixed != 0) {
        log_msg("fccc: unsafe or unknown state; retaining limit\n");
        return -1;
    }
    if (read_number(LIMIT_NODE, &current)) {
        log_msg("fccc: cannot read limit for release\n");
        return -1;
    }
    if (current == 0) {
        *last = -1;
        return 0;
    }
    if (current != *last) {
        log_msg("fccc: limit changed externally; not restoring\n");
        return -1;
    }
    if (write_number(LIMIT_NODE, RELEASE_MA)) {
        log_msg("fccc: failed to release limit\n");
        return -1;
    }
    *last = -1;
    return 0;
}

void __attribute__((noreturn)) run(long argc, char **argv)
{
    long last = -1;
    long lock_fd;
    long volt_cap = BASE_MA;
    int temp_limited = 0, volt_limited = 0, error = 0, write_ticks = 0;
    (void)argv;
    if (argc != 1 || setsig(2) || setsig(15)) {
        log_msg("fccc: unexpected arguments or signal setup failure\n");
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    lock_fd = sc(SYS_openat, -100, (s64)LOCK_PATH, 2 | 0100 | 0400000, 0600);
    if (lock_fd < 0 || sc(SYS_flock, lock_fd, 2 | 4, 0, 0) < 0) {
        log_msg("fccc: cannot acquire instance lock\n");
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    log_msg("fccc: started\n");
    while (!stop_requested) {
        long fixed, temp, voltage, capacity, temp_ma, volt_ma, current, target;
        int state = charging();
        if (read_number(FIXED_NODE, &fixed) || fixed != 0) {
            log_msg("fccc: fixed_temp unavailable or active; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (state < 0) {
            log_msg("fccc: charging status unavailable; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (!state) {
            if (release_limit(&last)) { error = 1; break; }
            temp_limited = 0;
            volt_limited = 0;
            volt_cap = BASE_MA;
            write_ticks = 0;
            sleep_second();
            continue;
        }
        if (read_number(TEMP_NODE, &temp) || temp < -400 || temp > 1250 ||
            read_number(FIXED_NODE, &fixed) || fixed != 0) {
            log_msg("fccc: invalid or overridden battery temperature; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (read_number(VOLT_NODE, &voltage) || voltage < 2000000 || voltage > 5000000) {
            log_msg("fccc: invalid or unavailable single-cell battery voltage; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (read_number(LIMIT_NODE, &current)) {
            log_msg("fccc: cannot read current limit; stopping\n");
            error = 1;
            break;
        }
        if (read_number(CAP_NODE, &capacity) || capacity < 0 || capacity > 100) {
            log_msg("fccc: invalid or unavailable battery capacity; stopping, retaining limit\n");
            error = 1;
            break;
        }
        /*  // 检查 被其它程序修改的情况
            if ((last < 0 && current != 0 && current != RELEASE_MA) ||
                (last >= 0 && current != last)) {
                log_msg("fccc: limit already in use or changed externally; stopping\n");
                error = 1;
                break;
            }
        */
        int write_needed;
        temp_ma = current_for_temp(temp, &temp_limited);
        volt_cap = cap_for_voltage(voltage, volt_cap, &volt_limited);
        volt_ma = volt_cap;
        target = temp_ma < volt_ma ? temp_ma : volt_ma;
        /* Above the capacity threshold the forced current is disabled entirely. */
        if (capacity > CAP_RELEASE) target = RELEASE_MA;
        /* A write issued before fast charging activates may not take effect,
         * so rewrite the target every 5 seconds even when it is unchanged. */
        write_needed = target != current || ++write_ticks >= 5;
        log_limit(temp, voltage, capacity, temp_ma, volt_ma, temp_limited,
                  volt_limited, current, target, write_needed);
        if (write_needed) {
            write_ticks = 0;
            if (read_number(FIXED_NODE, &fixed) || fixed != 0) {
                log_msg("fccc: fixed_temp changed; stopping, retaining limit\n");
                error = 1;
                break;
            }
            if (write_number(LIMIT_NODE, target)) {
                log_msg("fccc: limit write or readback failed; stopping\n");
                error = 1;
                break;
            }
            last = target;
        }
        sleep_second();
    }
    if (!error && release_limit(&last)) error = 1;
    log_msg(error ? "fccc: stopped with error\n" : "fccc: stopped\n");
    sc(SYS_exit, error, 0, 0, 0);
    __builtin_unreachable();
}

#ifndef HOSTTEST
void __attribute__((naked)) _start(void)
{
    asm volatile("ldr x0, [sp]\nadd x1, sp, #8\nb run");
}
#endif
