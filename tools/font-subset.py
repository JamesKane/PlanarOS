#!/usr/bin/env python3
"""tools/font-subset.py -- the system fonts' subsets (v8-plan step 1)

font.library embeds System Sans (Inter) and System Mono (JetBrains Mono),
each Regular and Bold, cut down from the originals in
tools/font-oracle/corpus/ to docs §14.3.1's repertoire. What's kept:
the outlines, the metrics, and the layout the engine reads (kern, liga,
tnum, pnum, and GDEF's classes). What's dropped: hinting (fpgm, prep,
cvt, gasp, glyphs' instructions), which the engine never runs, and every
other feature. Neither font names a Reserved Font Name under the OFL, so
the subsets keep their names.

    python3 tools/font-subset.py        # writes lib/font/fonts/*.ttf,
                                        # tools/font-fuzz/seeds/*.ttf and
                                        # the oracle's KernTest-Regular.ttf

Needs fontTools. The output is committed, so the build doesn't.
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, 'font-oracle', 'corpus')
OUT = os.path.join(HERE, '..', 'lib', 'font', 'fonts')

FONTS = [
    ('Inter-Regular.ttf', 'SystemSans-Regular.ttf'),
    ('Inter-Bold.ttf', 'SystemSans-Bold.ttf'),
    ('JetBrainsMono-Regular.ttf', 'SystemMono-Regular.ttf'),
    ('JetBrainsMono-Bold.ttf', 'SystemMono-Bold.ttf'),
]

# §14.3.1: Basic Latin, Latin-1, the punctuation English uses, the euro
# and trademark, box drawing and blocks (the Shell's), and U+FFFD.
REPERTOIRE = (list(range(0x20, 0x7f)) + list(range(0xa0, 0x100)) +
              list(range(0x2010, 0x203b)) + [0x20ac, 0x2122] +
              list(range(0x2500, 0x25a0)) + [0xfffd])


def cut(src, dst):
    opts = subset.Options()
    opts.layout_features = ['kern', 'liga', 'tnum', 'pnum']
    opts.legacy_kern = True             # a 'kern' table, if the font has one
    opts.hinting = False
    opts.drop_tables += ['fpgm', 'prep', 'cvt ', 'gasp', 'DSIG', 'BASE', 'STAT', 'meta']
    opts.name_IDs = ['*']
    opts.name_languages = ['*']
    opts.notdef_outline = True
    opts.glyph_names = False
    opts.recalc_bounds = True
    font = TTFont(src, recalcTimestamp=False)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=REPERTOIRE)
    sub.subset(font)
    font.save(dst)
    have = TTFont(dst).getBestCmap()
    missing = [c for c in REPERTOIRE if c not in have]
    print(f'{os.path.basename(dst)}: {os.path.getsize(dst)} bytes, '
          f'{len(have)} characters, {len(missing)} of the repertoire missing'
          + (f' (first {", ".join(f"U+{c:04X}" for c in missing[:6])})' if missing else ''))


# The fuzzer's seeds (tools/font-fuzz): each font cut to a few glyphs, a
# composite among them, kerning pairs (AV, To) and a ligature (ff, in
# Source Sans), so mutations reach the glyphs and the layout quickly.
SEEDS = os.path.join(HERE, 'font-fuzz', 'seeds')
SEED_TEXT = 'AÁVTo ffi1 é%'


def rename(font, family):
    """A Modified Version of a font with a Reserved Font Name (Source Sans
    3's 'Source') mustn't carry it (OFL 1.1, condition 3): its names made
    'family', the copyright and licence kept."""
    ps = family.replace(' ', '') + '-Regular'
    for rec in font['name'].names:
        new = {1: family, 3: ps, 4: family + ' Regular', 6: ps, 16: family, 21: family}.get(rec.nameID)
        if new is not None:
            rec.string = new
        elif rec.nameID not in (0, 7, 13, 14) and 'Source' in rec.toUnicode():
            rec.string = rec.toUnicode().replace('Source Sans 3', family).replace('Source', 'Planar')


def seed(src, dst):
    opts = subset.Options()
    opts.layout_features = ['kern', 'liga', 'tnum', 'pnum']
    opts.legacy_kern = True
    opts.hinting = False
    opts.drop_tables += ['fpgm', 'prep', 'cvt ', 'gasp', 'DSIG', 'BASE', 'STAT', 'meta']
    opts.name_IDs = [1, 2]
    opts.notdef_outline = True
    font = TTFont(src, recalcTimestamp=False)
    sub = subset.Subsetter(opts)
    sub.populate(text=SEED_TEXT)
    sub.subset(font)
    if 'Source' in font['name'].getDebugName(1):
        rename(font, 'Planar Test Sans')
    font.save(dst)
    print(f'{os.path.basename(dst)}: {os.path.getsize(dst)} bytes')


# The oracle's font for the paths no other font in the corpus takes
# (v8-plan step 2): the old kern table, and GSUB's lookups behind
# extensions. Source Sans 3 cut to Latin, its GPOS kerning between those
# characters written as a kern table's format 0 (GPOS dropped, so the
# engine falls back to it, as HarfBuzz does), and every GSUB lookup
# wrapped in an extension (type 7). Renamed: see rename().
KERN_TEST = os.path.join(CORPUS, 'KernTest-Regular.ttf')


def kern_test(src, dst):
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables import otTables as ot
    from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0

    opts = subset.Options()
    opts.layout_features = ['kern', 'liga', 'tnum', 'pnum']
    opts.hinting = False
    opts.drop_tables += ['fpgm', 'prep', 'cvt ', 'gasp', 'DSIG', 'BASE', 'STAT', 'meta']
    opts.name_IDs = ['*']
    opts.notdef_outline = True
    opts.glyph_names = True
    font = TTFont(src, recalcTimestamp=False)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=list(range(0x20, 0x7f)) + list(range(0xa0, 0x100)) + list(range(0x2010, 0x203b)))
    sub.subset(font)

    glyphs = set(font.getBestCmap().values())
    gpos = font['GPOS'].table
    kern_lookups = sorted({i for fr in gpos.FeatureList.FeatureRecord if fr.FeatureTag == 'kern'
                           for i in fr.Feature.LookupListIndex})
    pairs = {}
    for li in kern_lookups:
        for st in gpos.LookupList.Lookup[li].SubTable:
            st = getattr(st, 'ExtSubTable', st)
            if st.Format == 1:
                for g1, ps in zip(st.Coverage.glyphs, st.PairSet):
                    for r in ps.PairValueRecord:
                        xa = getattr(r.Value1, 'XAdvance', 0) if r.Value1 else 0
                        if g1 in glyphs and r.SecondGlyph in glyphs and xa:
                            pairs.setdefault((g1, r.SecondGlyph), xa)
            else:
                for g1 in st.Coverage.glyphs:
                    if g1 not in glyphs:
                        continue
                    row = st.Class1Record[st.ClassDef1.classDefs.get(g1, 0)].Class2Record
                    for g2 in sorted(glyphs):
                        v = row[st.ClassDef2.classDefs.get(g2, 0)].Value1
                        xa = getattr(v, 'XAdvance', 0) if v else 0
                        if xa:
                            pairs.setdefault((g1, g2), xa)
    table = KernTable_format_0()
    table.version, table.coverage, table.format = 0, 1, 0
    table.kernTable = dict(sorted(pairs.items())[:10000])  # a subtable's length is 16 bits
    font['kern'] = newTable('kern')
    font['kern'].version = 0
    font['kern'].kernTables = [table]
    del font['GPOS']

    gsub = font['GSUB'].table
    for lk in gsub.LookupList.Lookup:
        if lk.LookupType == 7:
            continue
        wrapped = []
        for st in lk.SubTable:
            ext = ot.ExtensionSubst()
            ext.Format, ext.ExtensionLookupType, ext.ExtSubTable = 1, lk.LookupType, st
            wrapped.append(ext)
        lk.SubTable, lk.LookupType = wrapped, 7
    rename(font, 'Planar Kern Test')
    font.save(dst)
    print(f'{os.path.basename(dst)}: {os.path.getsize(dst)} bytes, {len(table.kernTable)} kerning pairs, '
          f'{len(gsub.LookupList.Lookup)} GSUB lookups as extensions')


def main():
    os.makedirs(OUT, exist_ok=True)
    for src, dst in FONTS:
        cut(os.path.join(CORPUS, src), os.path.join(OUT, dst))
    os.makedirs(SEEDS, exist_ok=True)
    for src in ('Inter-Regular.ttf', 'JetBrainsMono-Regular.ttf', 'SourceSans3-Regular.ttf'):
        seed(os.path.join(CORPUS, src), os.path.join(SEEDS, src))
    kern_test(os.path.join(CORPUS, 'SourceSans3-Regular.ttf'), KERN_TEST)
    seed(KERN_TEST, os.path.join(SEEDS, 'KernTest-Regular.ttf'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
