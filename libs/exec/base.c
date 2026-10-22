/*
 * libs/exec/base.c -- exec.library: its base, its interface, OpenLibrary
 *
 * V6 step 4, docs §7.2. The shared library is lib/exec's pools, RawDoFmt
 * and CreateTask, the kernel calls' stubs and lib/rt's memory functions,
 * built as one image of code and constants (lib.ld) that the kernel
 * relocates once at boot into the shared library region. Its "main"
 * interface lists them. CopyMem, CopyMemQuick and SetMem are the word
 * loops unless the CPU has a vector unit, when the kernel points them at
 * arch/<isa>/user/string.S's instead (the alternatives below).
 *
 * The directory is the kernel's to fill, at boot, with every library's
 * base: it's read through volatile, since this code sees it as zeros.
 */
#include <planar/libimage.h>

constexpr unsigned DIRECTORY_SLOTS = 16;

[[gnu::section(".rodata.directory")]]
static struct Library *const volatile directory[DIRECTORY_SLOTS];

/* arch/<isa>/user/string.S, renamed for this image. */
void *vec_memcpy(void *restrict dst, const void *restrict src, size_t n);
void *vec_memmove(void *dst, const void *src, size_t n);
void *vec_memset(void *dst, int c, size_t n);

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static struct Library *OpenLibrary(const char *name, uint32_t version)
{
    struct Library *lib;

    if (!name)
        return nullptr;
    for (unsigned i = 0; i < DIRECTORY_SLOTS && (lib = directory[i]); i++)
        if (same(lib->lib_Name, name))
            return lib->lib_Version >= version ? lib : nullptr;
    return nullptr;
}

/* Nothing is unloaded in V6, and the region is read-only: no count. */
static void CloseLibrary(struct Library *library)
{
    (void)library;
}

static struct Interface *GetInterface(struct Library *library, const char *name,
                                      uint32_t version)
{
    if (!library || !name)
        return nullptr;
    for (struct Interface *const *i = library->lib_Interfaces; *i; i++)
        if (same((*i)->if_Name, name))
            return (*i)->if_Version >= version ? *i : nullptr;
    return nullptr;
}

static void DropInterface(struct Interface *iface)
{
    (void)iface;
}

static void copy_vector(const void *source, void *dest, uint64_t size)
{
    vec_memmove(dest, source, size);
}

static void copy_quick_vector(const void *source, void *dest, uint64_t size)
{
    vec_memcpy(dest, source, size);
}

static void *set_vector(void *dest, uint8_t fill, uint64_t size)
{
    return vec_memset(dest, fill, size);
}

static const struct Library base;

static const struct ExecIFace main_iface = {
    .Data = { "main", 1, sizeof(struct ExecIFace), (struct Library *)&base },

    .OpenLibrary = OpenLibrary,
    .CloseLibrary = CloseLibrary,
    .GetInterface = GetInterface,
    .DropInterface = DropInterface,

    .KLog = KLog,
    .ExitProcess = ExitProcess,
    .SetSignal = SetSignal,
    .Wait = Wait,
    .Signal = Signal,
    .FindTask = FindTask,
    .CreateTask = CreateTask,
    .ExitTask = ExitTask,
    .AllocSignal = AllocSignal,
    .FreeSignal = FreeSignal,

    .AllocArea = AllocArea,
    .FreeArea = FreeArea,
    .ShareArea = ShareArea,
    .UnshareArea = UnshareArea,
    .ReleaseArea = ReleaseArea,
    .ProtectArea = ProtectArea,
    .QueryArea = QueryArea,

    .CreateProcess = CreateProcess,
    .CreateProcessImage = CreateProcessImage,
    .StartProcess = StartProcess,
    .KillProcess = KillProcess,
    .QueryProcess = QueryProcess,
    .CloseHandle = CloseHandle,

    .CurrentTime = CurrentTime,
    .ArmTimer = ArmTimer,
    .QuerySystem = QuerySystem,
    .WaitAddr = WaitAddr,
    .WakeAddr = WakeAddr,

    .CopyMem = CopyMem,                 /* the word loops, until the kernel says */
    .CopyMemQuick = CopyMemQuick,
    .SetMem = SetMem,

    .CreatePool = CreatePool,
    .CreatePoolMem = CreatePoolMem,
    .DeletePool = DeletePool,
    .AllocPooled = AllocPooled,
    .FreePooled = FreePooled,
    .AllocVec = AllocVec,
    .FreeVec = FreeVec,

    .RawDoFmt = RawDoFmt,
};

static struct Interface *const interfaces[] = { (struct Interface *)&main_iface, nullptr };

static const struct Library base = {
    .lib_Name = "exec.library",
    .lib_Version = 6,
    .lib_Revision = 4,
    .lib_IdString = "exec.library 6.4 (22.10.2026)",
    .lib_Interfaces = interfaces,
};

static const struct LibraryAlt alts[] = {
    { (void *const *)&main_iface.CopyMem, (void *)copy_vector, LIBF_VECTOR },
    { (void *const *)&main_iface.CopyMemQuick, (void *)copy_quick_vector, LIBF_VECTOR },
    { (void *const *)&main_iface.SetMem, (void *)set_vector, LIBF_VECTOR },
};

const struct LibraryImage _library_image = {
    .li_Library = (struct Library *)&base,
    .li_Alts = alts,
    .li_NumAlts = sizeof alts / sizeof alts[0],
    .li_Directory = directory,
    .li_DirectorySlots = DIRECTORY_SLOTS,
};
