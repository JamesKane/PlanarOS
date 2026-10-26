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

    python3 tools/font-subset.py        # writes lib/font/fonts/*.ttf
                                        # and tools/font-fuzz/seeds/*.ttf

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
    font = TTFont(src)
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
# composite among them, so mutations reach the glyphs quickly.
SEEDS = os.path.join(HERE, 'font-fuzz', 'seeds')
SEED_TEXT = 'AÁfi1 é%'


def seed(src, dst):
    opts = subset.Options()
    opts.layout_features = ['kern', 'liga', 'tnum', 'pnum']
    opts.legacy_kern = True
    opts.hinting = False
    opts.drop_tables += ['fpgm', 'prep', 'cvt ', 'gasp', 'DSIG', 'BASE', 'STAT', 'meta']
    opts.name_IDs = [1, 2]
    opts.notdef_outline = True
    font = TTFont(src)
    sub = subset.Subsetter(opts)
    sub.populate(text=SEED_TEXT)
    sub.subset(font)
    font.save(dst)
    print(f'{os.path.basename(dst)}: {os.path.getsize(dst)} bytes')


def main():
    os.makedirs(OUT, exist_ok=True)
    for src, dst in FONTS:
        cut(os.path.join(CORPUS, src), os.path.join(OUT, dst))
    os.makedirs(SEEDS, exist_ok=True)
    for src in ('Inter-Regular.ttf', 'JetBrainsMono-Regular.ttf', 'SourceSans3-Regular.ttf'):
        seed(os.path.join(CORPUS, src), os.path.join(SEEDS, src))
    return 0


if __name__ == '__main__':
    sys.exit(main())
