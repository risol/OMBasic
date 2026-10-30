This directory contains the compression and decompression implementation from
miniz 3.1.2, pinned at upstream commit
`77d0dce8627735138c51770d1799a1ef48f2117d`.

Upstream: https://github.com/richgel999/miniz
License: MIT / Unlicense; see `LICENSE` and the source-file notices.

OMBasic builds only the low-level DEFLATE implementation and the zlib-style
stream API. ZIP archive functions are disabled. Gzip framing and checksum
validation are implemented by OMBasic's bounded wrapper.
