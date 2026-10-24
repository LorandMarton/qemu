/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Broadcast TLB invalidation of a CPU waiting in WFE.
 *
 * The secondary CPU caches the translations of a data page and a code page
 * at fixed virtual addresses, then waits in WFE.  The primary maps the two
 * addresses to other pages, invalidates them with a broadcast TLBI and
 * wakes the secondary with SEV: both must now see the new pages.  Each
 * kind of broadcast TLBI is used in turn.
 *
 * While the secondary waits, the invalidations it is sent must not be
 * lost; and the primary, the only CPU executing, must take its own part
 * of the invalidation at once.  Run with -smp 2 on a machine with PSCI
 * over HVC (virt).
 */

#include <stdint.h>
#include <minilib.h>

#define PAGE            4096
#define N_PAGES         2

/* The test addresses, at 3 GiB: level 1 index 3, outside the RAM map. */
#define TEST_VA         0xc0000000ull
#define DATA_VA         TEST_VA
#define CODE_VA         (TEST_VA + PAGE)

#define DESC_TABLE      3ull
#define DESC_PAGE       3ull
#define DESC_AF         (1ull << 10)
#define DESC_SH_INNER   (3ull << 8)
#define DESC_NG         (1ull << 11)

#define PSCI_CPU_ON     0xc4000003u

static uint64_t l1[512] __attribute__((aligned(PAGE)));
static uint64_t l2[512] __attribute__((aligned(PAGE)));
static uint64_t l3[512] __attribute__((aligned(PAGE)));
static uint32_t data[N_PAGES][PAGE / 4] __attribute__((aligned(PAGE)));
static uint32_t code[N_PAGES][PAGE / 4] __attribute__((aligned(PAGE)));

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

/*
 * Round the primary asks for, and the last one the secondary finished,
 * with what it saw; accessed with __atomic builtins.
 */
static int round_asked;
static int round_done;
static uint32_t seen_data, seen_code;

static int errors;

/* LDAR and STLR: no LDAPR (FEAT_LRCPC), for cortex-a72 */
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

static uint64_t page_desc(void *pa)
{
    return (uintptr_t)pa | DESC_AF | DESC_SH_INNER | DESC_NG | DESC_PAGE;
}

/* "mov w0, #value; ret" */
static void write_code(uint32_t *p, uint32_t value)
{
    p[0] = 0x52800000 | value << 5;
    p[1] = 0xd65f03c0;
}

static uint32_t read_data(void)
{
    /* volatile: each check loads through the current translation. */
    return *(volatile uint32_t *)DATA_VA;
}

static uint32_t run_code(void)
{
    uint32_t (*fn)(void) = (uint32_t (*)(void))CODE_VA;

    return fn();
}

static void secondary_main(void)
{
    int done = 0;

    for (;;) {
        while (LOAD(round_asked) == done) {
            asm volatile("wfe" : : : "memory");
        }
        done = LOAD(round_asked);
        STORE(seen_data, read_data());
        STORE(seen_code, run_code());
        STORE(round_done, done);
        asm volatile("dsb ish\n\tsev" : : : "memory");
    }
}

/* Have the secondary run a round and return what it saw. */
static void secondary_round(int round)
{
    STORE(round_asked, round);
    asm volatile("dsb ish\n\tsev" : : : "memory");
    while (LOAD(round_done) != round) {
        asm volatile("wfe" : : : "memory");
    }
    /*
     * Leave the secondary time to reach WFE again before the next
     * invalidation: the case under test.
     */
    for (int i = 0; i < 20000; i++) {
        asm volatile("" : : : "memory");
    }
}

static void map(int page)
{
    l3[0] = page_desc(data[page]);
    l3[1] = page_desc(code[page]);
    asm volatile("dsb ishst" : : : "memory");
}

enum {
    TLBI_VAE1IS,
    TLBI_VALE1IS,
    TLBI_VAAE1IS,
    TLBI_VAALE1IS,
    TLBI_ASIDE1IS,
    TLBI_VMALLE1IS,
    TLBI_RVAE1IS,
    N_KINDS
};

static const char *const tlbi_name[N_KINDS] = {
    "vae1is", "vale1is", "vaae1is", "vaale1is", "aside1is", "vmalle1is",
    "rvae1is",
};

static void tlbi(int kind)
{
    /* ASID 0, that of TTBR0_EL1. Range: TG 4K, SCALE 0, NUM 0: 2 pages. */
    uint64_t range = 1ull << 46 | TEST_VA >> 12;

    switch (kind) {
    case TLBI_VAE1IS:
        asm volatile("tlbi vae1is, %0" : : "r"(DATA_VA >> 12));
        asm volatile("tlbi vae1is, %0" : : "r"(CODE_VA >> 12));
        break;
    case TLBI_VALE1IS:
        asm volatile("tlbi vale1is, %0" : : "r"(DATA_VA >> 12));
        asm volatile("tlbi vale1is, %0" : : "r"(CODE_VA >> 12));
        break;
    case TLBI_VAAE1IS:
        asm volatile("tlbi vaae1is, %0" : : "r"(DATA_VA >> 12));
        asm volatile("tlbi vaae1is, %0" : : "r"(CODE_VA >> 12));
        break;
    case TLBI_VAALE1IS:
        asm volatile("tlbi vaale1is, %0" : : "r"(DATA_VA >> 12));
        asm volatile("tlbi vaale1is, %0" : : "r"(CODE_VA >> 12));
        break;
    case TLBI_ASIDE1IS:
        asm volatile("tlbi aside1is, %0" : : "r"(0ull));
        break;
    case TLBI_VMALLE1IS:
        asm volatile("tlbi vmalle1is");
        break;
    case TLBI_RVAE1IS:
        asm volatile("tlbi rvae1is, %0" : : "r"(range));
        break;
    }
    asm volatile("dsb ish\n\tisb" : : : "memory");
}

static void check(const char *who, int kind, uint32_t d, uint32_t c,
                  uint32_t want)
{
    if (d != want || c != want) {
        ml_printf("FAIL: %s after tlbi %s: data %d, code %d, want %d\n",
                  who, tlbi_name[kind], d, c, want);
        errors++;
    }
}

static void setup(void)
{
    uint64_t *boot_l1;

    asm volatile("mrs %0, ttbr0_el1" : "=r"(boot_l1));
    boot_l1 = (uint64_t *)((uintptr_t)boot_l1 & ~(PAGE - 1ull));

    for (int i = 0; i < N_PAGES; i++) {
        data[i][0] = i + 1;
        write_code(code[i], i + 1);
    }
    /* The RAM identity map of boot.S, and the test pages. */
    for (int i = 0; i < 512; i++) {
        l1[i] = boot_l1[i];
    }
    l1[TEST_VA >> 30] = (uintptr_t)l2 | DESC_TABLE;
    l2[0] = (uintptr_t)l3 | DESC_TABLE;
    map(0);
    asm volatile("dsb ish\n\tic iallu\n\tdsb ish\n\t"
                 "msr ttbr0_el1, %0\n\tisb\n\t"
                 "tlbi vmalle1\n\tdsb nsh\n\tisb"
                 : : "r"(l1) : "memory");

    ctx.sp = (uintptr_t)secondary_stack + sizeof(secondary_stack);
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ctx.ttbr0));
    asm volatile("mrs %0, tcr_el1" : "=r"(ctx.tcr));
    asm volatile("mrs %0, mair_el1" : "=r"(ctx.mair));
    asm volatile("mrs %0, sctlr_el1" : "=r"(ctx.sctlr));
    asm volatile("mrs %0, vbar_el1" : "=r"(ctx.vbar));
    asm volatile("mrs %0, cpacr_el1" : "=r"(ctx.cpacr));
    ctx.main = secondary_main;
    asm volatile("dsb ish" : : : "memory");
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
    uint64_t isar0;
    int64_t ret;
    int round = 0;
    int page = 0;

    /* ID_AA64ISAR0_EL1.TLB: 2 for TLB range maintenance. */
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));

    setup();
    ret = psci_cpu_on(1, secondary_entry, &ctx);
    if (ret != 0) {
        ml_printf("FAIL: PSCI CPU_ON: %d\n", (int)ret);
        return 1;
    }

    for (int pass = 0; pass < 50; pass++) {
        for (int kind = 0; kind < N_KINDS; kind++) {
            if (kind == TLBI_RVAE1IS && ((isar0 >> 56) & 0xf) < 2) {
                continue;
            }
            /* Both CPUs cache the translations of the current page. */
            secondary_round(++round);
            check("secondary", kind, LOAD(seen_data), LOAD(seen_code),
                  page + 1);
            check("primary", kind, read_data(), run_code(), page + 1);

            page ^= 1;
            map(page);
            tlbi(kind);
            check("primary", kind, read_data(), run_code(), page + 1);
            secondary_round(++round);
            check("secondary", kind, LOAD(seen_data), LOAD(seen_code),
                  page + 1);
        }
    }

    if (errors) {
        ml_printf("FAIL: %d errors\n", errors);
        return 1;
    }
    ml_printf("PASS\n");
    return 0;
}
