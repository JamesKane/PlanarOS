/*
 * planar/acpid.h -- what acpid says to hwdb
 *
 * docs §9.4, docs/v4-plan.md step 4. When the firmware described the
 * machine by ACPI, hwdb starts acpid before it matches anything, with the
 * firmware's tables granted to it (GrantFirmware) and a token as its
 * argument. acpid connects to hwdb's port "hwdb.acpi" and says hello with
 * the token, which tells it where the RSDP is. It loads the namespace
 * with ACPICA (third_party/acpica, unchanged), runs the _INI methods,
 * and sends hwdb each device it finds (ACPID_DEVICE), then ACPID_DONE.
 * hwdb adds them to its graph, and only then matches it. acpid stays, to
 * run the methods drivers will need run (the CIX's power resources).
 *
 * AML can touch any address, through an operation region. acpid asks
 * hwdb for each window it touches (ACPID_REGION). hwdb answers that it's
 * the firmware's tables, which acpid already maps, or refuses memory,
 * the interrupt controller, the IOMMU and what a driver holds (R5), or
 * leases the window to acpid. A refused read gives all ones, a refused
 * write is dropped, and the device being described is marked.
 *
 * A device is a record of fixed size: strings are offsets into
 * ad_Strings, each ending in a NUL, and 0 is "". hwdb checks every
 * offset and count before it uses one.
 */
#ifndef PLANAR_ACPID_H
#define PLANAR_ACPID_H

#include <stdint.h>

#include <planar/msg.h>

constexpr unsigned ACPID_KEYS = 8;
constexpr unsigned ACPID_RES = 32;
constexpr unsigned ACPID_STRINGS = 1536;

enum : uint32_t {
    ACPID_HELLO = 1,        /* am_Token in; am_Base out: the RSDP */
    ACPID_DEVICE = 2,       /* am_Device in */
    ACPID_DONE = 3,         /* am_Count in: the devices sent */
    ACPID_REGION = 4,       /* am_Base, am_Size, am_Count (1: the probe's) in; am_Count, am_Lease out */
    ACPID_PROBED = 5,       /* am_Count in: 1 if each of hwdb's answers to the probe was right */
};

/* acpid's argument: the token, and in test mode (hwdb's) the probe: it
 * asks for windows hwdb must refuse, and some it may lease, and checks
 * the answers (servers/acpid/main.c). */
constexpr uint64_t ACPID_ARG_PROBE = 1ULL << 32;

/* ACPID_REGION's answer, in am_Count. */
enum : uint32_t {
    ACPID_LEASED = 1,       /* am_Lease is granted to acpid: map it */
    ACPID_TABLES = 2,       /* the firmware's tables: already mapped */
    ACPID_REFUSED = 3,      /* mn_Error says why */
};

/* AcpidRes.ar_Kind */
enum : uint8_t {
    ACPID_R_MMIO = 1,       /* registers: ar_Base, ar_Size */
    ACPID_R_IRQ,            /* ar_Base: the GSIV, or the line of ar_Source's controller */
    ACPID_R_GPIO,           /* ar_Base: the first pin, ar_Size pins, on ar_Source */
    ACPID_R_BUS,            /* ar_Base: the address on ar_Source's bus (I2C, SPI, UART) */
    ACPID_R_WINDOW,         /* a bridge's: addresses it decodes for its children */
};

/* AcpidRes.ar_Flags */
constexpr uint8_t ACPID_RF_EDGE = 1u << 0;
constexpr uint8_t ACPID_RF_LOW = 1u << 1;      /* active low, or falling */
constexpr uint8_t ACPID_RF_SHARED = 1u << 2;
constexpr uint8_t ACPID_RF_INT = 1u << 3;      /* GPIO: GpioInt, not GpioIo */

struct AcpidRes {
    uint8_t ar_Kind;
    uint8_t ar_Flags;
    uint16_t ar_Source;     /* the device it names (a path), or 0 */
    uint32_t ar_Pad;
    uint64_t ar_Base, ar_Size;
};

/* AcpidDevice.ad_Methods: what took a method to learn, and what else
 * it has (§9.6's count). */
constexpr uint32_t ACPID_M_HID = 1u << 0;
constexpr uint32_t ACPID_M_CID = 1u << 1;
constexpr uint32_t ACPID_M_UID = 1u << 2;
constexpr uint32_t ACPID_M_STA = 1u << 3;
constexpr uint32_t ACPID_M_CRS = 1u << 4;
constexpr uint32_t ACPID_M_DSD = 1u << 5;
constexpr uint32_t ACPID_M_INI = 1u << 6;       /* it has an _INI */
constexpr uint32_t ACPID_M_POWER = 1u << 7;     /* _PS0 or _PR0: power by method */
constexpr uint32_t ACPID_M_REGION = 1u << 8;    /* describing it touched a region */
constexpr uint32_t ACPID_M_REFUSED = 1u << 9;   /* ... that hwdb refused */
constexpr uint32_t ACPID_M_FAILED = 1u << 10;   /* a method failed */
constexpr uint32_t ACPID_M_DESCRIBE = ACPID_M_HID | ACPID_M_CID | ACPID_M_UID | ACPID_M_STA |
                                      ACPID_M_CRS | ACPID_M_DSD;

struct AcpidDevice {
    uint32_t ad_Status;             /* _STA; 0xf when it has none */
    uint32_t ad_Methods;
    uint16_t ad_Path;               /* "\_SB_.PCI0" */
    uint16_t ad_Uid;
    uint16_t ad_NKeys, ad_NRes;
    uint16_t ad_Keys[ACPID_KEYS];   /* _HID, then the _CIDs; PRP0001's _DSD compatible first */
    uint16_t ad_Seg, ad_Bbn;        /* a host bridge's _SEG and _BBN */
    uint32_t ad_Dropped;            /* resources that didn't fit, or of kinds not sent */
    uint64_t ad_Adr;                /* _ADR, or UINT64_MAX */
    struct AcpidRes ad_Res[ACPID_RES];
    char ad_Strings[ACPID_STRINGS];
};

struct AcpidMsg {
    struct Message am_Msg;
    uint32_t am_Op;
    uint32_t am_Token;
    uint32_t am_Count;
    Handle am_Lease;
    uint64_t am_Base, am_Size;
    struct AcpidDevice am_Device;
};

#endif /* PLANAR_ACPID_H */
