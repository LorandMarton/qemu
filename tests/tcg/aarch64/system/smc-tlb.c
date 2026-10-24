/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Self-modifying code through a cached TLB entry.
 *
 * Translating code from a page write-protects it in every TLB: a writable
 * entry of the page that is already in a TLB must take the slow path, so
 * that the write invalidates the translated code.  Write a function, run
 * it, rewrite it through the same mapping, and run it again, many times.
 */

#include <stdint.h>
#include <minilib.h>

#define PAGE            4096
#define BLOCK           0x200000
/* At 3 GiB: level 1 index 3, outside the RAM map */
#define BLOCK_VA        0xc0000000ull
/* A block of RAM after the test image, writable and executable at EL1 */
#define BLOCK_PA        ((1ull << 30) + 32 * 0x100000)
#define CODE            (3 * PAGE)
#define ROUNDS          1000

#define DESC_TABLE      3ull
#define DESC_BLOCK      1ull
#define DESC_AF         (1ull << 10)
#define DESC_SH_INNER   (3ull << 8)

static uint64_t l1[512] __attribute__((aligned(PAGE)));
static uint64_t l2[512] __attribute__((aligned(PAGE)));

/* "mov w0, #value; ret", then the cache maintenance for new code */
static void write_code(uint32_t value)
{
    uint32_t *p = (uint32_t *)(BLOCK_VA + CODE);

    p[0] = 0x52800000 | value << 5;
    p[1] = 0xd65f03c0;
    asm volatile("dc cvau, %0\n\t"
                 "dsb ish\n\t"
                 "ic ivau, %0\n\t"
                 "dsb ish\n\t"
                 "isb" : : "r"(p) : "memory");
}

static uint32_t run_code(void)
{
    return ((uint32_t (*)(void))(BLOCK_VA + CODE))();
}

int main(void)
{
    uint64_t *boot_l1;
    int errors = 0;

    /* The RAM identity map of boot.S, and the block at 3 GiB */
    asm volatile("mrs %0, ttbr0_el1" : "=r"(boot_l1));
    boot_l1 = (uint64_t *)((uintptr_t)boot_l1 & ~(PAGE - 1ull));
    for (int i = 0; i < 512; i++) {
        l1[i] = boot_l1[i];
    }
    l1[BLOCK_VA >> 30] = (uintptr_t)l2 | DESC_TABLE;
    l2[0] = BLOCK_PA | DESC_AF | DESC_SH_INNER | DESC_BLOCK;
    asm volatile("dsb ish\n\t"
                 "msr ttbr0_el1, %0\n\t"
                 "isb\n\t"
                 "tlbi vmalle1\n\t"
                 "dsb nsh\n\t"
                 "isb" : : "r"(l1) : "memory");

    for (uint32_t i = 1; i <= ROUNDS; i++) {
        uint32_t got;

        write_code(i);
        got = run_code();
        if (got != i) {
            ml_printf("FAIL: round %d: code returned %d\n", i, got);
            if (++errors == 10) {
                break;
            }
        }
    }

    if (errors) {
        ml_printf("FAIL: %d errors\n", errors);
        return 1;
    }
    ml_printf("PASS\n");
    return 0;
}
