/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A CPU with crypto=off reports no Cryptographic Extension in its ID
 * registers. Run on a CPU that has it, e.g. -cpu cortex-a72,crypto=off.
 */

#include <stdint.h>
#include <minilib.h>

static unsigned field(uint64_t reg, unsigned shift)
{
    return (reg >> shift) & 0xf;
}

int main(void)
{
    uint64_t isar0, isar5;

    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    asm volatile("mrs %0, id_isar5_el1" : "=r"(isar5));

    /* AES, SHA1, SHA2, SHA3, SM3, SM4 */
    if (field(isar0, 4) || field(isar0, 8) || field(isar0, 12) ||
        field(isar0, 32) || field(isar0, 36) || field(isar0, 40)) {
        ml_printf("FAIL: ID_AA64ISAR0_EL1 0x%lx\n", isar0);
        return 1;
    }
    /* AES, SHA1, SHA2 */
    if (field(isar5, 4) || field(isar5, 8) || field(isar5, 12)) {
        ml_printf("FAIL: ID_ISAR5_EL1 0x%lx\n", isar5);
        return 1;
    }
    ml_printf("PASS\n");
    return 0;
}
