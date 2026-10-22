/*
 * lib/utility/tags.c -- tag lists and hooks (docs §17.4.3)
 *
 * Commodore's V36 calls, with 64-bit data. A list is items to TAG_DONE,
 * TAG_MORE carrying on in another list and TAG_IGNORE and TAG_SKIP
 * stepping over items; every walk goes through NextTagItem.
 */
#include <planar/utility.h>

struct TagItem *NextTagItem(const struct TagItem **tagListPtr)
{
    const struct TagItem *t;

    if (!tagListPtr)
        return nullptr;
    while ((t = *tagListPtr)) {
        switch (t->ti_Tag) {
        case TAG_DONE:
            *tagListPtr = nullptr;
            return nullptr;
        case TAG_MORE:
            *tagListPtr = (const struct TagItem *)(uintptr_t)t->ti_Data;
            continue;
        case TAG_IGNORE:
            *tagListPtr = t + 1;
            continue;
        case TAG_SKIP:
            *tagListPtr = t + 1 + t->ti_Data;
            continue;
        }
        *tagListPtr = t + 1;
        return (struct TagItem *)t;
    }
    return nullptr;
}

struct TagItem *FindTagItem(Tag tagValue, const struct TagItem *tagList)
{
    struct TagItem *t;

    while ((t = NextTagItem(&tagList)))
        if (t->ti_Tag == tagValue)
            return t;
    return nullptr;
}

uint64_t GetTagData(Tag tagValue, uint64_t defaultVal, const struct TagItem *tagList)
{
    struct TagItem *t = FindTagItem(tagValue, tagList);

    return t ? t->ti_Data : defaultVal;
}

bool TagInArray(Tag tagValue, const Tag *tagArray)
{
    if (tagArray)
        for (; *tagArray != TAG_DONE; tagArray++)
            if (*tagArray == tagValue)
                return true;
    return false;
}

uint64_t CallHookPkt(struct Hook *hook, void *object, void *paramPacket)
{
    return hook && hook->h_Entry ? hook->h_Entry(hook, object, paramPacket) : 0;
}
