/*
 * lib/dos/path.c -- AddPart, FilePart and PathPart: V36's paths
 * (planar/dos.h)
 *
 * A path is "VOLUME:dir/file". Nothing here asks the server: these are
 * the strings only, so a path made may name nothing.
 */
#include <planar/dos.h>

static unsigned length(const char *s)
{
    unsigned n = 0;

    while (s[n])
        n++;
    return n;
}

char *FilePart(const char *path)
{
    const char *part = path;

    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == ':')
            part = p + 1;
    return (char *)part;
}

char *PathPart(const char *path)
{
    const char *slash = nullptr;

    for (const char *p = path; *p; p++)
        if (*p == '/')
            slash = p;
        else if (*p == ':')
            slash = nullptr;
    return slash ? (char *)slash : FilePart(path);
}

bool AddPart(char *dirName, const char *fileName, unsigned size)
{
    unsigned keep = length(dirName), n;
    bool sep;

    for (const char *p = fileName; *p; p++)
        if (*p == ':') {
            keep = 0;                   /* its own volume */
            if (p == fileName)          /* ":x": from the directory's */
                for (unsigned i = 0; dirName[i]; i++)
                    if (dirName[i] == ':') {
                        keep = i + 1;
                        break;
                    }
            fileName += p == fileName;
            break;
        }
    if (keep && dirName[keep - 1] == '/')
        keep--;
    for (; *fileName == '/'; fileName++) {  /* a step up, to the volume at most */
        unsigned up = 0;

        for (unsigned i = 0; i < keep; i++)
            if (dirName[i] == '/')
                up = i;
            else if (dirName[i] == ':')
                up = i + 1;
        keep = up;
    }
    n = length(fileName);
    sep = keep && n && dirName[keep - 1] != ':';
    if (keep + sep + n >= size)
        return false;
    if (sep)
        dirName[keep++] = '/';
    for (unsigned i = 0; i <= n; i++)
        dirName[keep + i] = fileName[i];
    return true;
}
