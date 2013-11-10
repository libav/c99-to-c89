c99-to-c89
==========

Tool to convert C99 code to MSVC-compatible C89. This tool was first developed
to compile [Libav](http://www.libav.org) and [FFmpeg](http://ffmpeg.org) with
Microsoft Visual C++.

Dependencies
============

c99-to-c89 is based on LibClang, any clang version from 3.1 is known to work.

Usage
=====

c99-to-c89 provides two programs: `c99conv` and `c99wrap`:
 - `c99conv` converts preprocessed C sources from C99 to MSVC-compatible C89.
 - `c99wrap` uses the C preprocessor, converts its output and feeds it to the C compiler.

An example of the usage of `c99wrap` is:

```sh
c99wrap cl.exe -Fo out.o in.c
```

Binaries
========
Windows 32-bit builds of releases are available from:
http://download.videolan.org/pub/contrib/c99-to-c89/
