# Third-party code

Most of the library is written from scratch against a spec. These headers are ports of, or take
their API shape from, other people's code. Each keeps a note at the top of the header and the
original licence text is in `THIRD_PARTY/`.

| header | derived from | author | licence |
|---|---|---|---|
| `core_arena_alloc.h` | [tsoding/arena](https://github.com/tsoding/arena) | Alexey Kutepov | MIT, `LICENSE.arena` |
| `core_base.h` (String_View section) | [tsoding/sv](https://github.com/tsoding/sv) | Alexey Kutepov | MIT, `LICENSE.sv` |
| `util_uuid4.h` | [rxi/uuid4](https://github.com/rxi/uuid4) | rxi | MIT, `LICENSE.uuid4` |
| `core_log.h` | [rxi/log.c](https://github.com/rxi/log.c) | rxi | MIT, `LICENSE.log.c` |
| `codec_ini_dec.h` | [rxi/ini](https://github.com/rxi/ini) | rxi | MIT, `LICENSE.ini` |
| `codec_tinytar.h` | [rxi/microtar](https://github.com/rxi/microtar) | rxi | MIT, `LICENSE.microtar` |
| `codec_png_dec.h`, `codec_jpeg_dec.h`, `codec_deflate.h` | [stb_image](https://github.com/nothings/stb) | Sean Barrett | MIT or public domain, `LICENSE.stb` |
| `codec_mp3_dec.h` | dr_mp3 in [mackron/dr_libs](https://github.com/mackron/dr_libs) | David Reid | public domain or MIT-0, `LICENSE.dr_libs` |
| `core_ring_buffer.h` | ring buffer from c89atomic ([mackron/c89atomic](https://github.com/mackron/c89atomic)) | David Reid | public domain or MIT-0 (as stated in the header) |
| `util_exe_path.h` | [gpakosz/whereami](https://github.com/gpakosz/whereami) | Gregory Pakosz | WTFPL-2 or MIT, `LICENSE.whereami` |
| `util_path.h` | API shape of [likle/cwalk](https://github.com/likle/cwalk), not a port | Leonard Ikle | MIT, `LICENSE.cwalk` |
| `math_noise.h` | [Auburn/FastNoiseLite](https://github.com/Auburn/FastNoiseLite) | Jordan Peck and contributors | MIT, `LICENSE.FastNoiseLite` |
| `util_cli.h` | xgetopt | its authors | public domain (Unlicense), stated in the header |
| `math_curve.h` | the scalar Bezier formula from [recp/cglm](https://github.com/recp/cglm) | Recep Aslantas | MIT, `LICENSE.cglm` |

Implementations written from scratch from the published algorithm, not ports: `hash/hash_marmur.h`
(MurmurHash3 by Austin Appleby, public domain), `hash/hash_tinyxxhash.h` (XXH32 by Yann Collet),
`core_corountines.h` (the protothreads technique by Adam Dunkels), `util_fuzzy_match.h` (the FuzzyMatchV2
scoring of [junegunn/fzf](https://github.com/junegunn/fzf), MIT).
