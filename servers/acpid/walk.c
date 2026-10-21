/*
 * servers/acpid/walk.c -- the namespace, as hwdb wants it (docs §9.4)
 *
 * Loads the tables and the namespace with ACPICA, runs the _INI methods,
 * and describes each device: its path, _HID and _CIDs (or, for PRP0001,
 * the "compatible" of its _DSD), _UID, _STA, _ADR, a host bridge's _SEG
 * and _BBN, and what its _CRS gives (registers, interrupts, GPIO pins,
 * bus addresses). Paths are ACPICA's full ones, each segment four
 * characters ("\_SB_.PCI0"), and so are the paths resources name.
 *
 * It also counts what took running AML to learn (§9.6): a device whose
 * _HID, _CID, _UID, _STA, _CRS or _DSD is a method, not a name, and one
 * whose description touched an operation region.
 *
 * Operation regions in memory are acpid's own (memory() below), not
 * ACPICA's default handler: each window is asked of the platform
 * (acpid_map), which on Planar asks hwdb.
 */
#include "acpi.h"

#include "acpid.h"

struct acpid_count acpid_count;

static struct AcpidDevice *current;     /* being described: what a region access marks */
static uint32_t init_marks;             /* the same, while _INI and _REG ran */

static void mark(uint32_t m)
{
    if (current)
        current->ad_Methods |= m;
    else
        init_marks |= m;
}

/* --- Operation regions in memory. --- */

static ACPI_STATUS region_setup(ACPI_HANDLE, UINT32 function, void *, void **context)
{
    *context = function == ACPI_REGION_DEACTIVATE ? NULL : (void *)1;
    return AE_OK;
}

static UINT64 ones(UINT32 bits)
{
    return bits >= 64 ? ~0ULL : (1ULL << bits) - 1;
}

static ACPI_STATUS memory(UINT32 function, ACPI_PHYSICAL_ADDRESS at, UINT32 bits, UINT64 *value,
                          void *, void *)
{
    UINT32 bytes = bits / 8;
    bool refused = false;
    volatile void *p;

    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        return AE_AML_OPERAND_VALUE;
    mark(ACPID_M_REGION);
    p = acpid_map(at, bytes, &refused);
    if (!p) {
        mark(ACPID_M_REFUSED);
        if (function == ACPI_READ)
            *value = ones(bits);
        return AE_OK;
    }
    /* An access the region's own base leaves unaligned goes by bytes. */
    if (at & (bytes - 1)) {
        volatile uint8_t *b = (volatile uint8_t *)p;

        if (function == ACPI_READ) {
            *value = 0;
            for (UINT32 i = 0; i < bytes; i++)
                *value |= (UINT64)b[i] << (8 * i);
        } else {
            for (UINT32 i = 0; i < bytes; i++)
                b[i] = (uint8_t)(*value >> (8 * i));
        }
        return AE_OK;
    }
    if (function == ACPI_READ) {
        switch (bits) {
        case 8:  *value = *(volatile uint8_t *)p; break;
        case 16: *value = *(volatile uint16_t *)p; break;
        case 32: *value = *(volatile uint32_t *)p; break;
        default: *value = *(volatile uint64_t *)p; break;
        }
    } else {
        switch (bits) {
        case 8:  *(volatile uint8_t *)p = (uint8_t)*value; break;
        case 16: *(volatile uint16_t *)p = (uint16_t)*value; break;
        case 32: *(volatile uint32_t *)p = (uint32_t)*value; break;
        default: *(volatile uint64_t *)p = *value; break;
        }
    }
    return AE_OK;
}

static bool failed(const char *what, ACPI_STATUS s)
{
    char line[120];

    snprintf(line, sizeof line, "acpid: %s: %s", what, AcpiFormatException(s));
    acpid_say(line);
    return false;
}

bool acpid_load(void)
{
    ACPI_STATUS s;

    if (ACPI_FAILURE(s = AcpiInitializeSubsystem()))
        return failed("AcpiInitializeSubsystem", s);
    if (ACPI_FAILURE(s = AcpiInitializeTables(NULL, 32, FALSE)))
        return failed("AcpiInitializeTables", s);
    /* Before AcpiLoadTables installs the default, which then stands aside. */
    s = AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT, ACPI_ADR_SPACE_SYSTEM_MEMORY, memory,
                                       region_setup, NULL);
    if (ACPI_FAILURE(s))
        return failed("AcpiInstallAddressSpaceHandler", s);
    if (ACPI_FAILURE(s = AcpiLoadTables()))
        return failed("AcpiLoadTables", s);
    if (ACPI_FAILURE(s = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION)))
        return failed("AcpiEnableSubsystem", s);
    if (ACPI_FAILURE(s = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION)))
        return failed("AcpiInitializeObjects", s);
    return true;
}

/* --- Describing a device. --- */

uint16_t acpid_string(struct AcpidDevice *d, const char *s, size_t n)
{
    size_t used = 1;                /* offset 0 is "" */

    if (n == 0)
        return 0;
    while (used < ACPID_STRINGS && d->ad_Strings[used]) {
        size_t len = strlen(d->ad_Strings + used);

        if (len == n && memcmp(d->ad_Strings + used, s, n) == 0)
            return (uint16_t)used;
        used += len + 1;
    }
    if (used + n + 1 > ACPID_STRINGS)
        return 0;
    memcpy(d->ad_Strings + used, s, n);
    d->ad_Strings[used + n] = '\0';
    return (uint16_t)used;
}

static void key(struct AcpidDevice *d, const char *s, size_t n)
{
    uint16_t at;

    if (n == 0 || d->ad_NKeys == ACPID_KEYS)
        return;
    if (!(at = acpid_string(d, s, n)))
        return;
    for (unsigned i = 0; i < d->ad_NKeys; i++)
        if (d->ad_Keys[i] == at)
            return;
    d->ad_Keys[d->ad_NKeys++] = at;
}

/* Whether 'name' under 'h' is a method; and whether it's there at all. */
static bool is_method(ACPI_HANDLE h, const char *name, bool *there)
{
    ACPI_HANDLE c;
    ACPI_OBJECT_TYPE t;

    *there = ACPI_SUCCESS(AcpiGetHandle(h, (char *)name, &c)) &&
             ACPI_SUCCESS(AcpiGetType(c, &t));
    return *there && t == ACPI_TYPE_METHOD;
}

static void method_marks(struct AcpidDevice *d, ACPI_HANDLE h)
{
    static const struct { const char *name; uint32_t bit; } describe[] = {
        { "_HID", ACPID_M_HID }, { "_CID", ACPID_M_CID }, { "_UID", ACPID_M_UID },
        { "_STA", ACPID_M_STA }, { "_CRS", ACPID_M_CRS }, { "_DSD", ACPID_M_DSD },
        { "_INI", ACPID_M_INI },
    };
    static const char *const power[] = { "_PS0", "_PR0" };
    bool there;

    for (unsigned i = 0; i < sizeof describe / sizeof describe[0]; i++)
        if (is_method(h, describe[i].name, &there))
            d->ad_Methods |= describe[i].bit;
    for (unsigned i = 0; i < sizeof power / sizeof power[0]; i++) {
        is_method(h, power[i], &there);
        if (there)
            d->ad_Methods |= ACPID_M_POWER;
    }
}

static bool integer(ACPI_HANDLE h, const char *name, UINT64 *v)
{
    ACPI_OBJECT o;
    ACPI_BUFFER b = { sizeof o, &o };
    ACPI_STATUS s = AcpiEvaluateObjectTyped(h, (char *)name, NULL, &b, ACPI_TYPE_INTEGER);

    if (ACPI_FAILURE(s)) {
        if (s != AE_NOT_FOUND)
            mark(ACPID_M_FAILED);
        return false;
    }
    *v = o.Integer.Value;
    return true;
}

/* A path a resource names, made full from 'h's scope; the string as it
 * is if it names nothing. */
static uint16_t source(struct AcpidDevice *d, ACPI_HANDLE h, const ACPI_RESOURCE_SOURCE *rs)
{
    char path[128];
    ACPI_BUFFER b = { sizeof path, path };
    ACPI_HANDLE to;

    if (!rs->StringPtr || !rs->StringLength)
        return 0;
    if (ACPI_SUCCESS(AcpiGetHandle(h, rs->StringPtr, &to)) &&
        ACPI_SUCCESS(AcpiGetName(to, ACPI_FULL_PATHNAME, &b)))
        return acpid_string(d, path, strlen(path));
    return acpid_string(d, rs->StringPtr, strlen(rs->StringPtr));
}

struct crs {
    struct AcpidDevice *d;
    ACPI_HANDLE h;
    bool bridge;                    /* a host bridge: its address ranges are windows */
};

static void res(struct AcpidDevice *d, uint8_t kind, uint8_t flags, uint16_t src, uint64_t base,
                uint64_t size)
{
    struct AcpidRes *r;

    if (d->ad_NRes == ACPID_RES) {
        d->ad_Dropped++;
        return;
    }
    r = &d->ad_Res[d->ad_NRes++];
    r->ar_Kind = kind;
    r->ar_Flags = flags;
    r->ar_Source = src;
    r->ar_Base = base;
    r->ar_Size = size;
}

static uint8_t irq_flags(UINT8 triggering, UINT8 polarity, UINT8 shareable)
{
    return (uint8_t)((triggering == ACPI_EDGE_SENSITIVE ? ACPID_RF_EDGE : 0) |
                     (polarity == ACPI_ACTIVE_LOW ? ACPID_RF_LOW : 0) |
                     (shareable == ACPI_SHARED ? ACPID_RF_SHARED : 0));
}

static void address(struct crs *c, UINT8 type, UINT8 pc, UINT64 min, UINT64 offset, UINT64 len)
{
    if (type != ACPI_MEMORY_RANGE || len == 0) {
        c->d->ad_Dropped++;         /* bus numbers, I/O ports */
        return;
    }
    res(c->d, c->bridge || pc == ACPI_PRODUCER ? ACPID_R_WINDOW : ACPID_R_MMIO, 0, 0,
        min + offset, len);
}

static ACPI_STATUS one_resource(ACPI_RESOURCE *r, void *ctx)
{
    struct crs *c = ctx;
    struct AcpidDevice *d = c->d;

    switch (r->Type) {
    case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
        res(d, ACPID_R_MMIO, 0, 0, r->Data.FixedMemory32.Address,
            r->Data.FixedMemory32.AddressLength);
        break;
    case ACPI_RESOURCE_TYPE_MEMORY32:
        res(d, ACPID_R_MMIO, 0, 0, r->Data.Memory32.Minimum, r->Data.Memory32.AddressLength);
        break;
    case ACPI_RESOURCE_TYPE_MEMORY24:
        res(d, ACPID_R_MMIO, 0, 0, (uint64_t)r->Data.Memory24.Minimum << 8,
            (uint64_t)r->Data.Memory24.AddressLength << 8);
        break;
    case ACPI_RESOURCE_TYPE_ADDRESS16:
        address(c, r->Data.Address16.ResourceType, r->Data.Address16.ProducerConsumer,
                r->Data.Address16.Address.Minimum, r->Data.Address16.Address.TranslationOffset,
                r->Data.Address16.Address.AddressLength);
        break;
    case ACPI_RESOURCE_TYPE_ADDRESS32:
        address(c, r->Data.Address32.ResourceType, r->Data.Address32.ProducerConsumer,
                r->Data.Address32.Address.Minimum, r->Data.Address32.Address.TranslationOffset,
                r->Data.Address32.Address.AddressLength);
        break;
    case ACPI_RESOURCE_TYPE_ADDRESS64:
        address(c, r->Data.Address64.ResourceType, r->Data.Address64.ProducerConsumer,
                r->Data.Address64.Address.Minimum, r->Data.Address64.Address.TranslationOffset,
                r->Data.Address64.Address.AddressLength);
        break;
    case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
        address(c, r->Data.ExtAddress64.ResourceType, r->Data.ExtAddress64.ProducerConsumer,
                r->Data.ExtAddress64.Address.Minimum,
                r->Data.ExtAddress64.Address.TranslationOffset,
                r->Data.ExtAddress64.Address.AddressLength);
        break;
    case ACPI_RESOURCE_TYPE_IRQ:
        for (unsigned i = 0; i < r->Data.Irq.InterruptCount; i++)
            res(d, ACPID_R_IRQ,
                irq_flags(r->Data.Irq.Triggering, r->Data.Irq.Polarity, r->Data.Irq.Shareable), 0,
                r->Data.Irq.Interrupts[i], 1);
        break;
    case ACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
        const ACPI_RESOURCE_EXTENDED_IRQ *x = &r->Data.ExtendedIrq;
        uint16_t src = source(d, c->h, &x->ResourceSource);

        if (x->ProducerConsumer == ACPI_PRODUCER && !src) {
            d->ad_Dropped++;
            break;
        }
        for (unsigned i = 0; i < x->InterruptCount; i++)
            res(d, ACPID_R_IRQ, irq_flags(x->Triggering, x->Polarity, x->Shareable), src,
                x->Interrupts[i], 1);
        break;
    }
    case ACPI_RESOURCE_TYPE_GPIO: {
        const ACPI_RESOURCE_GPIO *g = &r->Data.Gpio;
        uint8_t flags = g->ConnectionType == ACPI_RESOURCE_GPIO_TYPE_INT
                            ? (uint8_t)(ACPID_RF_INT |
                                        irq_flags(g->Triggering, g->Polarity, g->Shareable))
                            : 0;

        if (g->PinTableLength)
            res(d, ACPID_R_GPIO, flags, source(d, c->h, &g->ResourceSource), g->PinTable[0],
                g->PinTableLength);
        break;
    }
    case ACPI_RESOURCE_TYPE_SERIAL_BUS: {
        const ACPI_RESOURCE_COMMON_SERIALBUS *s = &r->Data.CommonSerialBus;
        uint64_t at = 0;

        if (s->Type == ACPI_RESOURCE_SERIAL_TYPE_I2C)
            at = r->Data.I2cSerialBus.SlaveAddress;
        else if (s->Type == ACPI_RESOURCE_SERIAL_TYPE_SPI)
            at = r->Data.SpiSerialBus.DeviceSelection;
        res(d, ACPID_R_BUS, 0, source(d, c->h, &s->ResourceSource), at, s->Type);
        break;
    }
    case ACPI_RESOURCE_TYPE_END_TAG:
        break;
    default:
        d->ad_Dropped++;            /* I/O ports, DMA channels, pin functions */
        break;
    }
    return AE_OK;
}

/* PRP0001's keys are its _DSD's "compatible" (ACPI 6.5 §6.2.5; the
 * device-properties UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301). */
static void dsd_compatible(struct AcpidDevice *d, ACPI_HANDLE h)
{
    static const uint8_t uuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01,
    };
    ACPI_BUFFER b = { ACPI_ALLOCATE_BUFFER, NULL };
    const ACPI_OBJECT *dsd;

    if (ACPI_FAILURE(AcpiEvaluateObjectTyped(h, "_DSD", NULL, &b, ACPI_TYPE_PACKAGE)))
        return;
    dsd = b.Pointer;
    for (UINT32 i = 0; i + 1 < dsd->Package.Count; i += 2) {
        const ACPI_OBJECT *u = &dsd->Package.Elements[i], *props = &dsd->Package.Elements[i + 1];

        if (u->Type != ACPI_TYPE_BUFFER || u->Buffer.Length != 16 ||
            memcmp(u->Buffer.Pointer, uuid, 16) != 0 || props->Type != ACPI_TYPE_PACKAGE)
            continue;
        for (UINT32 k = 0; k < props->Package.Count; k++) {
            const ACPI_OBJECT *p = &props->Package.Elements[k], *name, *v;

            if (p->Type != ACPI_TYPE_PACKAGE || p->Package.Count != 2)
                continue;
            name = &p->Package.Elements[0];
            v = &p->Package.Elements[1];
            if (name->Type != ACPI_TYPE_STRING || strcmp(name->String.Pointer, "compatible"))
                continue;
            if (v->Type == ACPI_TYPE_STRING)
                key(d, v->String.Pointer, v->String.Length);
            else if (v->Type == ACPI_TYPE_PACKAGE)
                for (UINT32 j = 0; j < v->Package.Count; j++)
                    if (v->Package.Elements[j].Type == ACPI_TYPE_STRING)
                        key(d, v->Package.Elements[j].String.Pointer,
                            v->Package.Elements[j].String.Length);
        }
    }
    ACPI_FREE(b.Pointer);
}

static bool is_bridge(const struct AcpidDevice *d)
{
    for (unsigned i = 0; i < d->ad_NKeys; i++) {
        const char *k = d->ad_Strings + d->ad_Keys[i];

        if (!strcmp(k, "PNP0A03") || !strcmp(k, "PNP0A08"))
            return true;
    }
    return false;
}

static void describe(struct AcpidDevice *d, ACPI_HANDLE h)
{
    char path[128];
    ACPI_BUFFER pb = { sizeof path, path };
    ACPI_DEVICE_INFO *info = NULL;
    UINT64 v;

    d->ad_Status = 0xf;
    d->ad_Adr = UINT64_MAX;
    if (ACPI_SUCCESS(AcpiGetName(h, ACPI_FULL_PATHNAME, &pb)))
        d->ad_Path = acpid_string(d, path, strlen(path));
    method_marks(d, h);
    if (ACPI_SUCCESS(AcpiGetObjectInfo(h, &info))) {
        bool prp = info->Valid & ACPI_VALID_HID && !strcmp(info->HardwareId.String, "PRP0001");

        if (prp)
            dsd_compatible(d, h);
        if (info->Valid & ACPI_VALID_HID && !prp)
            key(d, info->HardwareId.String, info->HardwareId.Length - 1);
        if (info->Valid & ACPI_VALID_CID)
            for (UINT32 i = 0; i < info->CompatibleIdList.Count; i++)
                key(d, info->CompatibleIdList.Ids[i].String,
                    info->CompatibleIdList.Ids[i].Length - 1);
        if (info->Valid & ACPI_VALID_UID)
            d->ad_Uid = acpid_string(d, info->UniqueId.String, info->UniqueId.Length - 1);
        if (info->Valid & ACPI_VALID_ADR)
            d->ad_Adr = info->Address;
        ACPI_FREE(info);
    } else {
        d->ad_Methods |= ACPID_M_FAILED;
    }
    if (integer(h, "_STA", &v))
        d->ad_Status = (uint32_t)v;
    if (is_bridge(d)) {
        if (integer(h, "_SEG", &v))
            d->ad_Seg = (uint16_t)v;
        if (integer(h, "_BBN", &v))
            d->ad_Bbn = (uint16_t)v;
    }
    if (d->ad_Status & ACPI_STA_DEVICE_PRESENT) {
        struct crs c = { d, h, is_bridge(d) };
        ACPI_STATUS s = AcpiWalkResources(h, METHOD_NAME__CRS, one_resource, &c);

        if (ACPI_FAILURE(s) && s != AE_NOT_FOUND)
            d->ad_Methods |= ACPID_M_FAILED;
    }
}

struct walk {
    acpid_found *found;
    void *ctx;
    unsigned n;
    struct AcpidDevice d;
};

static void count(const struct AcpidDevice *d)
{
    acpid_count.devices++;
    if (d->ad_Status & ACPI_STA_DEVICE_PRESENT)
        acpid_count.present++;
    if (d->ad_Methods & ACPID_M_DESCRIBE)
        acpid_count.described_by_method++;
    if (d->ad_Methods & ACPID_M_POWER)
        acpid_count.power++;
    if (d->ad_Methods & ACPID_M_REGION)
        acpid_count.regions++;
    if (d->ad_Methods & ACPID_M_REFUSED)
        acpid_count.refused++;
    if (d->ad_Methods & ACPID_M_FAILED)
        acpid_count.failed++;
    for (unsigned b = 0; b < sizeof acpid_count.by / sizeof acpid_count.by[0]; b++)
        if (d->ad_Methods & 1u << b)
            acpid_count.by[b]++;
}

static ACPI_STATUS one_device(ACPI_HANDLE h, UINT32, void *ctx, void **)
{
    struct walk *w = ctx;

    memset(&w->d, 0, sizeof w->d);
    current = &w->d;
    describe(&w->d, h);
    current = NULL;
    count(&w->d);
    w->found(&w->d, w->ctx);
    w->n++;
    return AE_OK;
}

unsigned acpid_walk(acpid_found *found, void *ctx)
{
    static struct walk w;

    w = (struct walk){ found, ctx, 0, { 0 } };
    AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX, one_device, NULL, &w,
                      NULL);
    return w.n;
}

uint64_t acpid_gic(void)
{
    ACPI_TABLE_HEADER *t;
    const uint8_t *p;

    if (ACPI_FAILURE(AcpiGetTable(ACPI_SIG_MADT, 1, &t)))
        return 0;
    p = (const uint8_t *)t;
    for (UINT32 off = 44; off + 2 <= t->Length && p[off + 1] >= 2; off += p[off + 1])
        if (p[off] == ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR && off + 16 <= t->Length) {
            uint64_t base = 0;

            for (unsigned i = 0; i < 8; i++)
                base |= (uint64_t)p[off + 8 + i] << (8 * i);
            return base;
        }
    return 0;
}

void acpid_say_count(void)
{
    char line[200];
    const struct acpid_count *c = &acpid_count;

    snprintf(line, sizeof line,
             "acpid: %u devices, %u present; %u took a method to describe "
             "(_HID %u, _CID %u, _UID %u, _STA %u, _CRS %u, _DSD %u); %u have _INI, %u power by method",
             c->devices, c->present, c->described_by_method, c->by[0], c->by[1], c->by[2],
             c->by[3], c->by[4], c->by[5], c->by[6], c->power);
    acpid_say(line);
    snprintf(line, sizeof line,
             "acpid: describing them touched operation regions for %u (%u refused); "
             "methods failed for %u; _INI and _REG touched %s",
             c->regions, c->refused, c->failed,
             init_marks & ACPID_M_REFUSED  ? "regions, some refused"
             : init_marks & ACPID_M_REGION ? "regions"
                                           : "none");
    acpid_say(line);
}
