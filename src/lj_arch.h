/*
** Target architecture selection.
** Copyright (C) 2005-2010 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ARCH_H
#define _LJ_ARCH_H

#include "lua.h"

/* Target endianess. */
#define LUAJIT_LE	0
#define LUAJIT_BE	1

/* Target architectures. */
#define LUAJIT_ARCH_X86		1
#define LUAJIT_ARCH_x86		1
#define LUAJIT_ARCH_X64		2
#define LUAJIT_ARCH_x64		2
#define LUAJIT_ARCH_PPC		3
#define LUAJIT_ARCH_ppc		3
#define LUAJIT_ARCH_PPCSPE	4
#define LUAJIT_ARCH_ppcspe	4

/* Target OS. */
#define LUAJIT_OS_OTHER		0
#define LUAJIT_OS_WINDOWS	1
#define LUAJIT_OS_LINUX		2
#define LUAJIT_OS_OSX		3
#define LUAJIT_OS_BSD		4
#define LUAJIT_OS_POSIX		5

/* Select native target if no target defined. */
#ifndef LUAJIT_TARGET

#if defined(__i386) || defined(__i386__) || defined(_M_IX86)
#define LUAJIT_TARGET	LUAJIT_ARCH_X86
#elif defined(__x86_64__) || defined(__x86_64) || defined(_M_X64) || defined(_M_AMD64)
#define LUAJIT_TARGET	LUAJIT_ARCH_X64
#elif defined(__ppc__) || defined(__ppc) || defined(__PPC__) || defined(__PPC) || defined(__powerpc__) || defined(__powerpc) || defined(__POWERPC__) || defined(__POWERPC) || defined(_M_PPC)
#ifdef __NO_FPRS__
#define LUAJIT_TARGET	LUAJIT_ARCH_PPCSPE
#else
#define LUAJIT_TARGET	LUAJIT_ARCH_PPC
#endif
#else
#error "No support for this architecture (yet)"
#endif

#endif

/* Select native OS if no target OS defined. */
#ifndef LUAJIT_OS

#if defined(_WIN32)
#define LUAJIT_OS	LUAJIT_OS_WINDOWS
#elif defined(__linux__)
#define LUAJIT_OS	LUAJIT_OS_LINUX
#elif defined(__MACH__) && defined(__APPLE__)
#define LUAJIT_OS	LUAJIT_OS_OSX
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || \
      defined(__NetBSD__) || defined(__OpenBSD__)
#define LUAJIT_OS	LUAJIT_OS_BSD
#elif defined(__solaris__) || defined(__CYGWIN__)
#define LUAJIT_OS	LUAJIT_OS_POSIX
#else
#define LUAJIT_OS	LUAJIT_OS_OTHER
#endif

#endif

/* Set target OS properties. */
#if LUAJIT_OS == LUAJIT_OS_WINDOWS
#define LJ_OS_NAME	"Windows"
#elif LUAJIT_OS == LUAJIT_OS_LINUX
#define LJ_OS_NAME	"Linux"
#elif LUAJIT_OS == LUAJIT_OS_OSX
#define LJ_OS_NAME	"OSX"
#elif LUAJIT_OS == LUAJIT_OS_BSD
#define LJ_OS_NAME	"BSD"
#elif LUAJIT_OS == LUAJIT_OS_POSIX
#define LJ_OS_NAME	"Posix"
#else
#define LJ_OS_NAME	"Other"
#endif

#define LJ_TARGET_WINDOWS	(LUAJIT_OS == LUAJIT_OS_WINDOWS)
#define LJ_TARGET_LINUX		(LUAJIT_OS == LUAJIT_OS_LINUX)
#define LJ_TARGET_OSX		(LUAJIT_OS == LUAJIT_OS_OSX)
#define LJ_TARGET_POSIX		(LUAJIT_OS > LUAJIT_OS_WINDOWS)
#define LJ_TARGET_DLOPEN	LJ_TARGET_POSIX

/* Set target architecture properties. */
#if LUAJIT_TARGET == LUAJIT_ARCH_X86

#define LJ_ARCH_NAME		"x86"
#define LJ_ARCH_BITS		32
#define LJ_ARCH_ENDIAN		LUAJIT_LE
#define LJ_ARCH_BITENDIAN	LUAJIT_LE
#define LJ_ARCH_HASFPU		1
#define LJ_ABI_WIN		LJ_TARGET_WINDOWS
#define LJ_TARGET_X86		1
#define LJ_TARGET_X86ORX64	1
#define LJ_TARGET_EHRETREG	0
#define LJ_TARGET_MASKSHIFT	1
#define LJ_TARGET_MASKROT	1

#elif LUAJIT_TARGET == LUAJIT_ARCH_X64

#define LJ_ARCH_NAME		"x64"
#define LJ_ARCH_BITS		64
#define LJ_ARCH_ENDIAN		LUAJIT_LE
#define LJ_ARCH_BITENDIAN	LUAJIT_LE
#define LJ_ARCH_HASFPU		1
#define LJ_ABI_WIN		LJ_TARGET_WINDOWS
#define LJ_TARGET_X64		1
#define LJ_TARGET_X86ORX64	1
#define LJ_TARGET_EHRETREG	0
#define LJ_TARGET_MASKSHIFT	1
#define LJ_TARGET_MASKROT	1

#elif LUAJIT_TARGET == LUAJIT_ARCH_PPC

#error "No support for plain PowerPC CPUs (yet)"

#elif LUAJIT_TARGET == LUAJIT_ARCH_PPCSPE

#define LJ_ARCH_NAME		"ppcspe"
#define LJ_ARCH_BITS		32
#define LJ_ARCH_ENDIAN		LUAJIT_BE
#define LJ_ARCH_BITENDIAN	LUAJIT_BE
#define LJ_ARCH_HASFPU		1
#define LJ_ABI_SOFTFP		1
#define LJ_ABI_EABI		1
#define LJ_TARGET_PPC		1
#define LJ_TARGET_PPCSPE	1
#define LJ_TARGET_EHRETREG	3
#define LJ_TARGET_MASKSHIFT	0
#define LJ_TARGET_MASKROT	1
#define LJ_ARCH_NOJIT		1

#else
#error "No target architecture defined"
#endif

#ifndef LJ_PAGESIZE
#define LJ_PAGESIZE		4096
#endif

/* Check for minimum required compiler versions. */
#if defined(__GNUC__)
#if LJ_TARGET_X64
#if __GNUC__ < 4
#error "Need at least GCC 4.0 or newer"
#endif
#elif LJ_TARGET_PPC
#if (__GNUC__ < 4) || ((__GNUC__ == 4) && __GNUC_MINOR__ < 3)
#error "Need at least GCC 4.3 or newer"
#endif
#else
#if (__GNUC__ < 3) || ((__GNUC__ == 3) && __GNUC_MINOR__ < 4)
#error "Need at least GCC 3.4 or newer"
#endif
#endif
#endif

/* Check target-specific constraints. */
#ifndef _BUILDVM_H
#if LJ_TARGET_PPC
#if defined(_SOFT_FLOAT) || defined(_SOFT_DOUBLE)
#error "No support for PowerPC CPUs without double-precision FPU"
#endif
#if defined(_LITTLE_ENDIAN)
#error "No support for little-endian PowerPC"
#endif
#if defined(_LP64)
#error "No support for PowerPC 64 bit mode"
#endif
#endif
#endif

/* Disable or enable the JIT compiler. */
#if defined(LUAJIT_DISABLE_JIT) || defined(LJ_ARCH_NOJIT)
#define LJ_HASJIT		0
#else
#define LJ_HASJIT		1
#endif

#if LJ_ARCH_ENDIAN == LUAJIT_BE
#define LJ_ENDIAN_SELECT(le, be)	be
#define LJ_ENDIAN_LOHI(lo, hi)		hi lo
#else
#define LJ_ENDIAN_SELECT(le, be)	le
#define LJ_ENDIAN_LOHI(lo, hi)		lo hi
#endif

#if LJ_ARCH_BITS == 32
#define LJ_32			1
#define LJ_64			0
#else
#define LJ_32			0
#define LJ_64			1
#endif

#endif
