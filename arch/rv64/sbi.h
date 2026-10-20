/*
 * arch/rv64/sbi.h -- calls into the M-mode firmware (RISC-V SBI)
 */
#ifndef ARCH_RV64_SBI_H
#define ARCH_RV64_SBI_H

#include <stdint.h>

constexpr uint32_t SBI_EXT_BASE = 0x10;
constexpr uint32_t SBI_EXT_TIME = 0x54494d45;
constexpr uint32_t SBI_EXT_IPI  = 0x735049;
constexpr uint32_t SBI_EXT_HSM  = 0x48534d;

constexpr uint32_t SBI_BASE_GET_SPEC_VERSION = 0;
constexpr uint32_t SBI_BASE_PROBE_EXTENSION  = 3;
constexpr uint32_t SBI_TIME_SET_TIMER        = 0;
constexpr uint32_t SBI_IPI_SEND_IPI          = 0;
constexpr uint32_t SBI_HSM_HART_START        = 0;

struct sbiret {
    int64_t error;      /* 0 or a negative SBI_ERR_* */
    uint64_t value;
};

struct sbiret sbi_call(uint32_t ext, uint32_t fid, uint64_t a0, uint64_t a1, uint64_t a2);

/* Whether the firmware implements extension 'ext'. */
bool sbi_has(uint32_t ext);

#endif /* ARCH_RV64_SBI_H */
