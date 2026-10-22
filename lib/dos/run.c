/*
 * lib/dos/run.c -- dos.library's loader, RunCommand (planar/dos.h)
 */
#include <planar/dos.h>
#include <planar/syscalls.h>

constexpr uint64_t PAGE = 4096;
constexpr uint64_t IMAGE_MAX = 64ULL << 20;     /* a program's whole image, at most */

/* --- The loader (ELF-64, System V gABI). --- */

extern const uint16_t user_elf_machine;     /* arch/<arch>/user/elf.c */

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)p[i] << 8 * i;
    return v;
}

int64_t RunCommand(struct Dos *d, const char *path, const char *args, int64_t sigbit,
                   uint32_t flags)
{
    uint64_t arg = 0;
    uint8_t eh[64], ph[16 * 56];
    struct ProcessImage pi = { 0 };
    struct DosEntry de;
    uint64_t total = 0, etext = 0, phoff, entry;
    uint16_t phnum, phentsize;
    int64_t f, e, area;
    uint8_t *image;
    const char *base;

    if ((f = OpenFile(d, path, 0, &de)) < 0)
        return f;
    e = ERR_BADARG;
    if (ReadFile(d, (uint32_t)f, 0, eh, sizeof eh) != sizeof eh || eh[0] != 0x7f || eh[1] != 'E' ||
        eh[2] != 'L' || eh[3] != 'F' || eh[4] != 2 || eh[5] != 1 || le(eh + 16, 2) != 3 ||
        le(eh + 18, 2) != user_elf_machine)
        goto out;                   /* not a 64-bit little-endian PIE for this machine */
    entry = le(eh + 24, 8);
    phoff = le(eh + 32, 8);
    phentsize = (uint16_t)le(eh + 54, 2);
    phnum = (uint16_t)le(eh + 56, 2);
    if (phentsize != 56 || phnum == 0 || phnum > 16 || phoff > de.de_Size ||
        (uint64_t)phnum * 56 > de.de_Size - phoff ||
        ReadFile(d, (uint32_t)f, phoff, ph, phnum * 56u) != phnum * 56)
        goto out;
    /* Its size, and where the code ends: the first writable segment. */
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = ph + 56 * i;
        uint64_t va = le(p + 16, 8), filesz = le(p + 32, 8), memsz = le(p + 40, 8);

        if (le(p, 4) != 1)          /* PT_LOAD */
            continue;
        if (memsz < filesz || va > IMAGE_MAX || memsz > IMAGE_MAX - va ||
            le(p + 8, 8) > de.de_Size || filesz > de.de_Size - le(p + 8, 8))
            goto out;
        if (va + memsz > total)
            total = va + memsz;
        if (le(p + 4, 4) & 2 && (!etext || va < etext))    /* PF_W */
            etext = va;
    }
    total = (total + PAGE - 1) & ~(PAGE - 1);
    if (!etext)
        etext = total;
    if (total == 0 || etext % PAGE || entry >= etext)
        goto out;
    if ((area = AllocArea(total + PAGE, AREAF_EXEC, (void **)&image)) < 0) {
        e = area;
        goto out;
    }
    /* The command line, in the page past the image. */
    if (args) {
        char *line = (char *)image + total;
        unsigned i = 0;

        for (; args[i] && i < DOS_ARGS - 1; i++)
            line[i] = args[i];
        line[i] = '\0';
        arg = (uint64_t)line;
    }
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = ph + 56 * i;
        uint64_t filesz = le(p + 32, 8);

        if (le(p, 4) != 1 || filesz == 0)
            continue;
        if (ReadFile(d, (uint32_t)f, le(p + 8, 8), image + le(p + 16, 8), (uint32_t)filesz) !=
            (int64_t)filesz) {
            FreeArea((Handle)area);
            goto out;
        }
    }
    /* Its name: the path's last part. */
    base = FilePart(path);
    for (unsigned i = 0; i < sizeof pi.pi_Name - 1 && base[i]; i++)
        pi.pi_Name[i] = base[i];
    pi.pi_Area = (Handle)area;
    pi.pi_Etext = etext;
    pi.pi_Entry = entry;
    e = CreateProcessImage(&pi, arg, sigbit, flags);
    if (e < 0)
        FreeArea((Handle)area);     /* still ours if the kernel didn't take it */
out:
    CloseFile(d, (uint32_t)f);
    return e;
}
