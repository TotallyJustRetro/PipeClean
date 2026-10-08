# Third-Party Sources

PipeClean includes single-file third-party libraries used by the native runtime for image, font, and audio decoding.

## stb

Source: **nothings/stb**  
Repository: https://github.com/nothings/stb

| File | Version |
| --- | --- |
| `runtime/stb_image.h` | 2.30 |
| `runtime/stb_image_write.h` | 1.16 |
| `runtime/stb_truetype.h` | 1.26 |
| `runtime/stb_vorbis.inc` | 1.22 |

The stb project describes these libraries as public-domain or MIT-licensed single-file C/C++ libraries; individual files contain their applicable license text.

## dr_libs

Source: **mackron/dr_libs**  
Repository: https://github.com/mackron/dr_libs

| File | Version |
| --- | --- |
| `runtime/dr_flac.h` | 0.13.4 |
| `runtime/dr_mp3.h` | 0.7.4 |
| `runtime/dr_wav.h` | 0.14.6 |

dr_libs describes these single-file audio libraries as public domain or MIT-0, with the applicable license text included in each source file.

## Source integrity

The repository does not include commercial game ROMs. The third-party files above are software dependencies and are separate from any game ROM or proprietary game content.

When vendor sources are restored automatically, the project uses the upstream repositories listed above. Keep the version information in this document aligned with the version banners in the corresponding source files.
