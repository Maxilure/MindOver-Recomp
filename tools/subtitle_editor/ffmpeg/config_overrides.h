/* =============================================================================
 * tools/subtitle_editor/ffmpeg/config_overrides.h -- the SDK's FFmpeg
 * settings, with the Bink pieces the editor needs switched on
 * =============================================================================
 *
 * The SDK builds FFmpeg 4.4 for XMA only (its config.h: almost every codec
 * off, no assembly). The editor needs the Bink AUDIO decoder (the cartoons'
 * dialogue) and the Bink VIDEO decoder (watching a cartoon with its
 * subtitles). The files under tools/subtitle_editor/ffmpeg/ each read this
 * header and then compile ONE of FFmpeg's own files, unchanged; whatever
 * else those call is already in the SDK's libavcodec. The header guard of
 * the SDK's config_<system>.h keeps FFmpeg's own `#include "config.h"` from
 * putting the old values back.
 *
 * The CPU-specific code paths (ARCH_*) are switched off for the files built
 * here: plain C everywhere (the SDK's config has no assembler either), the
 * same result on Linux, Windows and ARM; fast enough for one 720p movie.
 * ========================================================================== */
#ifndef SUBTITLE_EDITOR_FFMPEG_CONFIG_OVERRIDES_H
#define SUBTITLE_EDITOR_FFMPEG_CONFIG_OVERRIDES_H
#include "config.h"
#undef CONFIG_BINKAUDIO_RDFT_DECODER
#undef CONFIG_BINKAUDIO_DCT_DECODER
#undef CONFIG_BINK_DECODER
#undef CONFIG_BLOCKDSP
#undef CONFIG_HPELDSP
#define CONFIG_BINKAUDIO_RDFT_DECODER 1
#define CONFIG_BINKAUDIO_DCT_DECODER 1
#define CONFIG_BINK_DECODER 1
#define CONFIG_BLOCKDSP 1
#define CONFIG_HPELDSP 1
#undef ARCH_X86
#undef ARCH_AARCH64
#undef ARCH_ARM
#define ARCH_X86 0
#define ARCH_AARCH64 0
#define ARCH_ARM 0
#endif
