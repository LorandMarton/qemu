/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SEV wakes a CPU waiting in WFE.
 *
 * Two CPUs take turns: each waits in WFE for the other to advance a
 * shared counter, then advances it and sends an event.  A lost wake-up
 * leaves both waiting and the test times out.  Run with -smp 2 on a
 * machine with PSCI over HVC (virt).
 */

#include <stdint.h>
#include <minilib.h>

#define ROUNDS          500000

#define PSCI_CPU_ON     0xc4000003u

/*
 * What the secondary needs to run C code with the primary's translation,
 * and the function to run: read by secondary_entry().
 */
struct secondary_ctx {
    uint64_t sp, ttbr0, tcr, mair, sctlr, vbar, cpacr;
    void (*main)(void);
};
static struct secondary_ctx ctx;
static uint8_t secondary_stack[16384] __attribute__((aligned(16)));

/* The turn, accessed with __atomic builtins: CPU n moves it on when n. */
static int turn;

/* LDAR and STLR: no exclusive access, so WFE sleeps */
#define LOAD(x)         __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define STORE(x, v)     __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

/* PSCI CPU_ON entry, MMU off, x0 = &ctx. */
static void __attribute__((naked)) secondary_entry(void)
{
    asm("ldp x1, x2, [x0]\n\t"          /* sp, ttbr0 */
        "mov sp, x1\n\t"
        "msr ttbr0_el1, x2\n\t"
        "ldp x1, x2, [x0, #16]\n\t"     /* tcr, mair */
        "msr tcr_el1, x1\n\t"
        "msr mair_el1, x2\n\t"
        "ldp x1, x2, [x0, #32]\n\t"     /* sctlr, vbar */
        "msr vbar_el1, x2\n\t"
        "ldp x2, x3, [x0, #48]\n\t"     /* cpacr, main */
        "msr cpacr_el1, x2\n\t"
        "tlbi vmalle1\n\t"
        "dsb nsh\n\t"
        "isb\n\t"
        "msr sctlr_el1, x1\n\t"
        "isb\n\t"
        "blr x3\n"
        "1:\n\t"
        "wfe\n\t"
        "b 1b");
}

static void wait_turn(int t)
{
    while (LOAD(turn) != t) {
        asm volatile("wfe" : : : "memory");
    }
}

static void play(int cpu)
{
    for (int t = cpu; t < 2 * ROUNDS; t += 2) {
        wait_turn(t);
        STORE(turn, t + 1);
        asm volatile("dsb ish\n\tsev" : : : "memory");
    }
}

static void secondary_main(void)
{
    play(1);
}

static int64_t psci_cpu_on(uint64_t mpidr, void (*entry)(void), void *arg)
{
    register uint64_t x0 asm("x0") = PSCI_CPU_ON;
    register uint64_t x1 asm("x1") = mpidr;
    register uint64_t x2 asm("x2") = (uintptr_t)entry;
    register uint64_t x3 asm("x3") = (uintptr_t)arg;

    asm volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

int main(void)
{
    int64_t ret;

    ctx.sp = (uintptr_t)secondary_stack + sizeof(secondary_stack);
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ctx.ttbr0));
    asm volatile("mrs %0, tcr_el1" : "=r"(ctx.tcr));
    asm volatile("mrs %0, mair_el1" : "=r"(ctx.mair));
    asm volatile("mrs %0, sctlr_el1" : "=r"(ctx.sctlr));
    asm volatile("mrs %0, vbar_el1" : "=r"(ctx.vbar));
    asm volatile("mrs %0, cpacr_el1" : "=r"(ctx.cpacr));
    ctx.main = secondary_main;
    asm volatile("dsb ish" : : : "memory");

    ret = psci_cpu_on(1, secondary_entry, &ctx);
    if (ret != 0) {
        ml_printf("FAIL: PSCI CPU_ON: %d\n", (int)ret);
        return 1;
    }
    play(0);
    wait_turn(2 * ROUNDS);
    ml_printf("PASS\n");
    return 0;
}
