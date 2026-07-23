/* MSVC host shim, force-included (/FIportab.h). Neutralizes GCC's __attribute__((packed)) on the
   radar wire structs so radar_render.c compiles on the host. Byte layout is irrelevant here -- the
   render/aggregation tests exercise field values and logic, not wire packing. */
#pragma once
#ifdef _MSC_VER
#define __attribute__(x)
#endif
