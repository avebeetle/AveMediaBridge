# Private Xiph sources for Ogg Vorbis import

AveMediaBridge links libogg 1.3.5 and libvorbis 1.3.7 privately and statically. The decoder uses libvorbisfile for bounded inspection of logical stream boundaries and the existing FFmpeg runtime for packet decoding and rate conversion. Nothing from the private Xiph API crosses the Bridge DLL boundary.

CMake verifies the original release archives against the SHA-256 values in [AveMediaBridgeVorbis.cmake](../../cmake/AveMediaBridgeVorbis.cmake). An offline build can pass `-DAVEMEDIABRIDGE_XIPH_ARCHIVES=<directory>` containing `libogg-1.3.5.tar.xz` and `libvorbis-1.3.7.tar.xz`.

The private patch widens byte offsets on Windows, propagates backward-search errors, frees partial link metadata on probe failure, bounds the link recursion, and records whether the first complete audio span agrees with an untrimmed zero origin. It is applied to copies in the build directory. The upstream headers and libraries retain their public ABI.

Redistribute both [libogg's license](libogg-1.3.5-COPYING.txt) and [libvorbis's license](libvorbis-1.3.7-COPYING.txt) with the product.
