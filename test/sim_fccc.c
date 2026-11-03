/* gcc -std=gnu11 -Wall -Wextra -Werror test/sim_fccc.c -lm -o sim_fccc */
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOSTTEST
#include "../fccc.c"

static jmp_buf exit_env;
static long fixed, temp, voltage, limit, written_values[8];
static int charging_state, fail_temp, fail_voltage, fail_write, fail_lock, ticks, scenario, writes;

enum { NORMAL, FIXED_ACTIVE, FIXED_ON_STOP, EXTERNAL_CHANGE, BAD_TEMP, WRITE_FAILURE, OFFLINE, OFFLINE_CLEARED, VOLT_RISE, BAD_VOLT };

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

long sim_sc(long n, long a, long b, long c, long d)
{
    (void)d;
    if (n == SYS_openat) {
        const char *path = (const char *)b;
        if (!strcmp(path, FIXED_NODE)) return 3;
        if (!strcmp(path, TEMP_NODE)) return fail_temp ? -1 : 4;
        if (!strcmp(path, VOLT_NODE)) return fail_voltage ? -1 : 5;
        if (!strcmp(path, LIMIT_NODE)) return 6;
        if (!strcmp(path, STATUS_NODE)) return 7;
        if (!strcmp(path, LOCK_PATH)) return 8;
        return -1;
    }
    if (n == SYS_flock) return fail_lock ? -1 : 0;
    if (n == SYS_read) {
        char text[64];
        long count;
        if (a == 7) snprintf(text, sizeof(text), "%s\n", charging_state ? "Charging" : "Discharging");
        else if (a == 3) snprintf(text, sizeof(text), "%ld\n", fixed);
        else if (a == 4) snprintf(text, sizeof(text), "%ld\n", temp);
        else if (a == 5) snprintf(text, sizeof(text), "%ld\n", voltage);
        else if (a == 6) snprintf(text, sizeof(text), "%ld\n", limit);
        else return -1;
        count = (long)strlen(text);
        if (count > c) count = c;
        memcpy((void *)b, text, (size_t)count);
        return count;
    }
    if (n == SYS_write) {
        if (a == 6) {
            char text[24];
            if (fail_write || c >= (long)sizeof(text)) return -1;
            memcpy(text, (void *)b, (size_t)c);
            text[c] = 0;
            limit = strtol(text, NULL, 10);
            check(writes < 8, "write history capacity");
            written_values[writes] = limit;
            writes++;
        }
        return c;
    }
    if (n == SYS_nanosleep) {
        ticks++;
        if (ticks == 1) {
            if (scenario == FIXED_ACTIVE) fixed = 300;
            else if (scenario == FIXED_ON_STOP) { fixed = 300; on_signal(15); }
            else if (scenario == EXTERNAL_CHANGE) limit = 1000;
            else if (scenario == BAD_TEMP) fail_temp = 1;
            else if (scenario == BAD_VOLT) fail_voltage = 1;
            else if (scenario == VOLT_RISE) voltage = VOLT_L3;
            else if (scenario == OFFLINE) charging_state = 0;
            else if (scenario == OFFLINE_CLEARED) { charging_state = 0; limit = 0; }
            else on_signal(15);
        } else {
            on_signal(15);
        }
        return 0;
    }
    if (n == SYS_exit) longjmp(exit_env, (int)a + 1);
    return 0;
}

static void reset(int which)
{
    fixed = 0; temp = 320; voltage = 4200000; limit = 0;
    charging_state = 1; fail_temp = 0; fail_write = 0; fail_lock = 0;
    ticks = 0; scenario = which; writes = 0; stop_requested = 0;
    fail_voltage = 0;
}

static int execute(void)
{
    char *args[] = { "fccc", NULL };
    int result = setjmp(exit_env);
    if (!result) run(1, args);
    return result - 1;
}

static void test_formula(void)
{
    int t, limited = 0;
    check(current_for_temp(451, &limited) == 15600 && !limited, "L1 boundary");
    check(current_for_temp(452, &limited) == 14100 && limited, "L1 entry");
    check(current_for_temp(450, &limited) == 15000 && limited, "hysteresis");
    check(current_for_temp(441, &limited) == 15600 && !limited, "exit boundary");
    for (t = 452; t <= 500; t++) {
        int ct = t > 480 ? 480 : t;
        int expected = (int)(15000 * pow(0.94, ct - 451));
        if (ct > 473) expected = (int)(expected * pow(0.85, ct - 473));
        if (expected < 2000) expected = 2000;
        limited = 0;
        check(current_for_temp(t, &limited) == expected, "reference formula");
    }
}

static void test_voltage_formula(void)
{
    long v, previous = 15000;
    int limited = 0;
    check(current_for_voltage(VOLT_L1, &limited) == BASE_MA && !limited, "voltage L1 boundary");
    check(current_for_voltage(4310000, &limited) == 14100 && limited, "voltage 10mV step");
    check(current_for_voltage(4290000, &limited) == 15000 && limited, "voltage hysteresis");
    check(current_for_voltage(VOLT_EXIT, &limited) == BASE_MA && !limited, "voltage exit boundary");
    for (v = VOLT_L1 + 1; v <= VOLT_L3 + 10000; v++) {
        long cv = v > VOLT_L3 ? VOLT_L3 : v;
        long actual;
        int expected = (int)(15000 * pow(0.94, (double)(cv - VOLT_L1) / VOLT_STEP));
        if (cv > VOLT_L2)
            expected = (int)(expected * pow(0.85, (double)(cv - VOLT_L2) / VOLT_STEP));
        if (expected < VOLT_MIN_MA) expected = VOLT_MIN_MA;
        limited = 0;
        actual = current_for_voltage(v, &limited);
        check(actual == expected, "continuous voltage reference formula");
        check(actual <= previous, "voltage current monotonic");
        previous = actual;
    }
    limited = 0;
    check(current_for_voltage(4500000, &limited) == VOLT_MIN_MA, "voltage floor");
}

int main(void)
{
    test_formula();
    test_voltage_formula();
    reset(VOLT_RISE);
    check(execute() == 0 && writes == 3 && written_values[0] == BASE_MA &&
          written_values[1] == VOLT_MIN_MA && written_values[2] == RELEASE_MA,
          "voltage dominates combined limit");
    reset(NORMAL);
    temp = TEMP_L3; voltage = VOLT_L3;
    check(execute() == 0 && written_values[0] == MIN_MA, "temperature lower floor wins");
    reset(BAD_VOLT);
    check(execute() == 1 && writes == 1 && limit == BASE_MA, "voltage read failure retains limit");
    reset(NORMAL);
    voltage = 0;
    check(execute() == 1 && writes == 0, "zero voltage rejected");
    reset(NORMAL);
    voltage = 9000000;
    check(execute() == 1 && writes == 0, "pack voltage rejected");
    reset(NORMAL);
    check(execute() == 0 && limit == RELEASE_MA && writes == 2, "normal stop restores");
    reset(FIXED_ACTIVE);
    check(execute() == 1 && limit == BASE_MA && writes == 1, "fixed temp stops without release");
    reset(FIXED_ACTIVE);
    temp = 480;
    check(execute() == 1 && limit == MIN_MA && writes == 1, "hot limit retained on fault");
    reset(FIXED_ON_STOP);
    check(execute() == 1 && limit == BASE_MA && writes == 1, "unsafe signal stop retains limit");
    reset(EXTERNAL_CHANGE);
    check(execute() == 1 && limit == 1000 && writes == 1, "external change preserved");
    reset(BAD_TEMP);
    check(execute() == 1 && limit == BASE_MA && writes == 1, "sensor error retains limit");
    reset(WRITE_FAILURE);
    fail_write = 1;
    check(execute() == 1 && writes == 0, "write failure reported");
    reset(OFFLINE);
    check(execute() == 0 && limit == RELEASE_MA && writes == 2, "unplug restores");
    reset(OFFLINE_CLEARED);
    check(execute() == 0 && limit == 0 && writes == 1, "unplug already cleared");
    reset(NORMAL);
    limit = 3000;
    check(execute() == 1 && limit == 3000 && writes == 0, "existing limit preserved");
    reset(NORMAL);
    fixed = 320;
    check(execute() == 1 && limit == 0 && writes == 0, "fixed temp blocks startup");
    reset(NORMAL);
    fail_lock = 1;
    check(execute() == 1 && writes == 0, "second instance rejected");
    puts("fccc tests passed");
    return 0;
}
