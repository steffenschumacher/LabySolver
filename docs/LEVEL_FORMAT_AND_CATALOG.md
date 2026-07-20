# Canonical rules and level catalog

This document is an independently written, connectivity-mask transcription of
game information supplied by the original author. The original source, assets,
archive, and prose are deliberately excluded from this repository.

## Rules established by the supplied material

The board is five columns by seven rows. Coordinates are zero-indexed `(x,y)`.
A move optionally places the ladybug anywhere in its current connected
component, rotates and inserts the spare tile, shifts one movable row or column,
then lets the ladybug traverse its new connected component.

Goals are ordered and attached to tiles, not fixed cells. After initialisation
and every insertion, advance through every consecutive goal whose tile is in
the ladybug's connected component. A later reachable goal cannot be collected
before the next required goal. Several consecutive goals can therefore be
collected after one insertion. Goals and the ladybug remain attached to an
ejected tile and return when that spare tile is inserted.

`pushOut=yes` permits the player tile to be ejected. When it is `no`, such a
candidate is illegal. Goal tiles may be ejected in either mode. `pushes` is an
upper bound, so a solution can use fewer moves.

The insertion coordinates are `(0,1)`, `(0,3)`, `(0,5)`, `(1,0)`, `(3,0)`,
`(1,6)`, `(3,6)`, `(4,1)`, `(4,3)`, and `(4,5)`.

## Connectivity encoding

Each hexadecimal digit is one tile: bit 0 north, bit 1 east, bit 2 south, and
bit 3 west. Thus `0` is blocked, `1` is a north dead end, `3` is a north/east
corner, `5` is a vertical corridor, `B` is a T-section without south, and `F`
is an intersection. Adjacent tiles connect only when both expose their shared
edge. A board below is seven five-digit rows separated by `/`; goals retain
their listed order.

| Level | Pushes | Push out | Board masks, top to bottom | Spare | Start | Ordered goals |
|---:|---:|:---:|---|:---:|:---:|---|
| 1 | 3 | yes | `00000/000A0/2AA0C/00005/60AA9/5A000/10000` | `0` | `0,6` | `0,2` |
| 2 | 2 | yes | `2AA0C/00005/06AA9/03AAC/006A9/55050/00100` | `0` | `2,6` | `1,3 0,0` |
| 3 | 2 | yes | `00400/02500/00900/00C00/00100/00000/00000` | `0` | `1,1` | `2,0 2,4` |
| 4 | 2 | yes | `00000/06800/69000/A0000/3A800/00000/00000` | `0` | `2,1` | `2,4` |
| 5 | 2 | yes | `02A58/00010/00000/9000B/00000/A0001/00000` | `0` | `4,0` | `3,1 1,0` |
| 6 | 2 | yes | `00000/00000/0060C/05A50/00309/000A0/00000` | `0` | `4,4` | `3,5 4,2 2,2 2,4` |
| 7 | 5 | yes | `6AA0C/A0A55/50005/00055/50005/00055/30A09` | `A` | `0,0` | `0,6 4,6 4,0 0,0` |
| 8 | 3 | yes | `00004/00000/00005/00609/003A8/00000/00000` | `0` | `4,4` | `4,0` |
| 9 | 3 | yes | `006A8/D5000/007AC/90100/002A9/00000/2A800` | `0` | `2,6` | `0,6 2,4 4,0` |
| 10 | 7 | yes | `00400/000F0/20108/00000/00100/00000/00000` | `0` | `2,2` | `2,0 0,2 2,4 4,2` |
| 11 | 2 | yes | `40000/3C00C/05E65/05590/052A5/03AA9/00000` | `A` | `2,4` | `2,3 0,0 4,1` |
| 12 | 7 | yes | `20AA8/00000/20000/00000/00AA8/0C005/003A8` | `A` | `0,0` | `4,0 0,2 4,4 4,6` |
| 13 | 5 | yes | `00400/05050/00500/00000/00550/00000/00100` | `0` | `2,6` | `2,4 2,2 2,0` |
| 14 | 4 | yes | `00000/00000/00400/050A0/20F08/00A50/00100` | `0` | `2,2` | `4,4 2,6 0,4 2,2` |
| 15 | 2 | yes | `006E0/06D00/0530C/03AAA/00005/0000F/00001` | `5` | `2,3` | `4,6 2,0` |
| 16 | 2 | yes | `06AC0/69503/503C1/50503/3AA90/00000/00000` | `0` | `2,2` | `2,3 0,4 1,0 4,2` |
| 17 | 2 | yes | `2AA0C/000A0/6AA09/550A0/3AA0C/000A5/2AA09` | `A` | `0,0` | `0,6` |
| 18 | 4 | yes | `00000/00000/00E08/02AC0/05B00/00208/00000` | `0` | `2,4` | `1,3 4,2 2,5 4,5` |
| 19 | 6 | yes | `00400/005E0/0072C/05096/06C05/05509/03BC9` | `0` | `4,4` | `2,0 3,2 2,6 4,6` |
| 20 | 7 | yes | `00000/24008/00000/05000/056AC/0590D/0F000` | `0` | `2,4` | `1,1 1,6 0,1 4,1` |
| 21 | 4 | yes | `00000/002C0/06290/0360A/00C00/005A0/00100` | `0` | `2,2` | `2,1 4,3 2,6` |
| 22 | 6 | yes | `00000/000A4/002A9/06A0A/50000/00000/100A8` | `0` | `2,2` | `4,1 4,6 1,3 0,6` |
| 23 | 7 | no | `00008/00000/20A00/0E0A0/05A08/B00A0/20000` | `0` | `0,6` | `4,4 0,2 4,0` |
| 24 | 3 | yes | `00000/00000/00E7C/00950/006C5/00399/00000` | `A` | `3,2` | `4,5 3,5 2,2` |
| 25 | 6 | yes | `00000/0A0A0/20A0C/00050/00001/020C0/00000` | `0` | `0,2` | `1,1 4,2 4,4 1,5` |
| 26 | 3 | yes | `00A00/C650C/3C05C/00503/00469/05B90/03000` | `0` | `2,4` | `2,0` |
| 27 | 6 | yes | `6A64C/4A594/76AC5/C3ACC/76369/A569E/36B39` | `7` | `2,2` | `2,6 0,6 0,2 4,6` |
| 28 | 5 | yes | `00000/00000/405F0/003E0/05501/16D00/10160` | `5` | `2,4` | `0,2 0,6 4,4 0,5` |
| 29 | 6 | yes | `00000/4A000/3A800/A0AC0/00005/00000/2A001` | `0` | `2,2` | `0,1 0,6 3,3 4,6` |
| 30 | 6 | yes | `01400/00B0C/06005/9CA00/60F55/10309/05000` | `0` | `0,5` | `2,4 4,4 4,1 1,0` |
| 31 | 7 | yes | `40000/50000/30000/C0000/794D0/0FAC3/00389` | `5` | `2,4` | `0,4 3,6 0,0 2,5` |
| 32 | 6 | yes | `65AC4/5C395/36FC5/A69D2/55045/A4B05/36BA9` | `5` | `1,5` | `0,6 2,3 4,0 1,5` |
| 33 | 6 | no | `00090/A06A0/052AC/AC05A/00309/00600/02900` | `0` | `2,2` | `2,4 1,6 3,0` |
| 34 | 7 | yes | `00000/A800A/60AAC/00A50/10005/06000/0EA89` | `0` | `2,6` | `1,6 0,4 3,6 4,2` |
| 35 | 7 | no | `40000/00000/3C000/30C00/003C0/00305/00001` | `9` | `0,0` | `4,6` |
| 36 | 7 | yes | `00C50/04000/53500/00C90/10680/07A00/0A308` | `0` | `0,4` | `1,1 2,2 3,4 4,6` |
| 37 | 7 | no | `40AAC/556A0/5FCA0/75C06/39358/103B0/00008` | `0` | `4,4` | `2,0 0,0 4,6 0,5` |
| 38 | 7 | yes | `04000/A03C0/06900/065A0/054A4/00369/00009` | `0` | `1,3` | `1,0 3,4 4,6 4,4` |
| 39 | 7 | no | `6AA0C/50AC0/30051/2E3C3/06F09/5AAE5/39890` | `5` | `2,4` | `2,6 0,3 0,2 4,2` |
| 40 | 6 | no | `656AC/CA45A/6C71D/C5F06/796C1/354DC/39359` | `C` | `2,4` | `4,0 0,6 2,2 4,2` |

The machine-readable equivalent is `sketch/LevelCatalog.hpp`. Corpus tests
check that all 40 entries load, validate, and retain the expected depth,
goal-count, and player-ejection distributions.
