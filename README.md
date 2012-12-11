c99-to-c89
==========

Tool to convert C99 code to MSVC-compatible C89

Dependencies
============

c99-to-c89 is based on LibClang, any clang version from 3.1 is known to work.

Usage
=====

c99conv converts preprocessed C sources, the provided c99wrap uses the C preprocessor,
converts its output and feeds it to the C compiler.

c99wrap $CC $CFLAGS source 
