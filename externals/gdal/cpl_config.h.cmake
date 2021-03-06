/* Define if you want to use pthreads based multiprocessing support */
#cmakedefine CPL_MULTIPROC_PTHREAD

/* Define to 1 if you have the `PTHREAD_MUTEX_RECURSIVE' constant. */
#cmakedefine HAVE_PTHREAD_MUTEX_RECURSIVE 1

/* --prefix directory for GDAL install */
#cmakedefine GDAL_PREFIX @GDAL_PREFIX@

/* Define to 1 if you have the <assert.h> header file. */
#cmakedefine HAVE_ASSERT_H 1

/* Define to 1 if you have the `atoll' function. */
#cmakedefine HAVE_ATOLL 1

/* Define to 1 if you have the <csf.h> header file. */
#cmakedefine HAVE_CSF_H 1

/* Define to 1 if you have the <dbmalloc.h> header file. */
#cmakedefine HAVE_DBMALLOC_H 1

/* Define to 1 if you have the declaration of `strtof', and to 0 if you don't.
   */
#cmakedefine HAVE_DECL_STRTOF 1

/* Define to 1 if you have the <direct.h> header file. */
#cmakedefine HAVE_DIRECT_H 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#cmakedefine HAVE_DLFCN_H 1

/* Define to 1 if you don't have `vprintf' but do have `_doprnt.' */
#cmakedefine HAVE_DOPRNT 1

/* Define to 1 if you have the <errno.h> header file. */
#cmakedefine HAVE_ERRNO_H 1

/* Define to 1 if you have the <fcntl.h> header file. */
#cmakedefine HAVE_FCNTL_H 1

/* Define to 1 if you have the <float.h> header file. */
#cmakedefine HAVE_FLOAT_H 1

/* Define to 1 if you have the `getcwd' function. */
#cmakedefine HAVE_GETCWD 1

/* Define if you have the iconv() function and it works. */
#cmakedefine HAVE_ICONV

/* Define as 0 or 1 according to the floating point format suported by the
   machine */
#cmakedefine01 HAVE_IEEEFP

/* Define to 1 if the system has the type `int16'. */
#cmakedefine HAVE_INT16 1

/* Define to 1 if the system has the type `int32'. */
#cmakedefine HAVE_INT32 1

/* Define to 1 if the system has the type `int8'. */
#cmakedefine HAVE_INT8 1

/* Define to 1 if you have the <inttypes.h> header file. */
#cmakedefine HAVE_INTTYPES_H 1

/* Define to 1 if you have the <jpeglib.h> header file. */
#cmakedefine HAVE_JPEGLIB_H 1

/* Define to 1 if you have the `dl' library (-ldl). */
#cmakedefine HAVE_LIBDL 1

/* Define to 1 if you have the `m' library (-lm). */
#cmakedefine HAVE_LIBM 1

/* Define to 1 if you have the `pq' library (-lpq). */
#cmakedefine HAVE_LIBPQ 1

/* Define to 1 if you have the `rt' library (-lrt). */
#cmakedefine HAVE_LIBRT 1

/* Define to 1 if you have the <limits.h> header file. */
#cmakedefine HAVE_LIMITS_H 1

/* Define to 1 if you have the <locale.h> header file. */
#cmakedefine HAVE_LOCALE_H 1

/* Define to 1, if your compiler supports long long data type */
#cmakedefine HAVE_LONG_LONG 1

/* Define to 1 if you have the <memory.h> header file. */
#cmakedefine HAVE_MEMORY_H 1

/* Define to 1 if you have the <png.h> header file. */
#cmakedefine HAVE_PNG_H 1

/* Define to 1 if you have the `snprintf' function. */
#cmakedefine HAVE_SNPRINTF 1

/* Define to 1 if you have the <stdint.h> header file. */
#cmakedefine HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#cmakedefine HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#cmakedefine HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#cmakedefine HAVE_STRING_H 1

/* Define to 1 if you have the `strtof' function. */
#cmakedefine HAVE_STRTOF 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#cmakedefine HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#cmakedefine HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#cmakedefine HAVE_UNISTD_H 1

/* Define to 1 if you have the <values.h> header file. */
#cmakedefine HAVE_VALUES_H 1

/* Define to 1 if you have the `vprintf' function. */
#cmakedefine HAVE_VPRINTF 1

/* Define to 1 if you have the `vsnprintf' function. */
#cmakedefine HAVE_VSNPRINTF 1

/* Define to 1 if you have the `readlink' function. */
#cmakedefine HAVE_READLINK 1

/* Define to 1 if you have the `posix_spawnp' function. */
#cmakedefine HAVE_POSIX_SPAWNP 1

/* Define to 1 if you have the `vfork' function. */
#cmakedefine HAVE_VFORK 1

/* Define to 1 if you have the `lstat' function. */
#cmakedefine HAVE_LSTAT 1

/* Set the native cpu bit order (FILLORDER_LSB2MSB or FILLORDER_MSB2LSB) */
#cmakedefine HOST_FILLORDER @HOST_FILLORDER@

/* Define as const if the declaration of iconv() needs const. */
#cmakedefine ICONV_CONST @ICONV_CONST@
#ifndef ICONV_CONST
#   define ICONV_CONST
#endif // !defined(ICONV_CONST)

/* For .cpp files, define as const if the declaration of iconv() needs const. */
#cmakedefine ICONV_CPP_CONST @ICONV_CPP_CONST@
#ifndef ICONV_CPP_CONST
#   define ICONV_CPP_CONST
#endif // !defined(ICONV_CPP_CONST)

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#cmakedefine LT_OBJDIR @LT_OBJDIR@

/* Define for Mac OSX Framework build */
#cmakedefine MACOSX_FRAMEWORK

/* The size of `int', as computed by sizeof. */
#cmakedefine SIZEOF_INT @SIZEOF_INT@

/* The size of `long', as computed by sizeof. */
#cmakedefine SIZEOF_LONG @SIZEOF_LONG@

/* The size of `unsigned long', as computed by sizeof. */
#cmakedefine SIZEOF_UNSIGNED_LONG @SIZEOF_UNSIGNED_LONG@

/* The size of `void*', as computed by sizeof. */
#cmakedefine SIZEOF_VOIDP @SIZEOF_VOIDP@

/* Define to 1 if you have the ANSI C header files. */
#cmakedefine STDC_HEADERS 1

/* Define to 1 if you have fseek64, ftell64 */
#cmakedefine UNIX_STDIO_64 1

/* Define to 1 if you want to use the -fvisibility GCC flag */
#cmakedefine USE_GCC_VISIBILITY_FLAG 1

/* Define to 1 if GCC atomic builtins are available */
#cmakedefine HAVE_GCC_ATOMIC_BUILTINS 1

/* Define to name of 64bit fopen function */
#cmakedefine VSI_FOPEN64 @VSI_FOPEN64@

/* Define to name of 64bit ftruncate function */
#cmakedefine VSI_FTRUNCATE64 @VSI_FTRUNCATE64@

/* Define to name of 64bit fseek func */
#cmakedefine VSI_FSEEK64 @VSI_FSEEK64@

/* Define to name of 64bit ftell func */
#cmakedefine VSI_FTELL64 @VSI_FTELL64@

/* Define to 1, if you have 64 bit STDIO API */
#cmakedefine VSI_LARGE_API_SUPPORTED 1

/* Define to 1, if you have LARGEFILE64_SOURCE */
#cmakedefine VSI_NEED_LARGEFILE64_SOURCE 1

/* Define to name of 64bit stat function */
#cmakedefine VSI_STAT64 @VSI_STAT64@

/* Define to name of 64bit stat structure */
#cmakedefine VSI_STAT64_T @VSI_STAT64_T@

/* Define to 1 if your processor stores words with the most significant byte
   first (like Motorola and SPARC, unlike Intel and VAX). */
#cmakedefine WORDS_BIGENDIAN 1

/* Define to 1 if you have the `getaddrinfo' function. */
#cmakedefine HAVE_GETADDRINFO 1

/* Use this file to override settings in instances where you're doing FAT compiles
   on Apple.  It is currently off by default because it doesn't seem to work with 
   newish ( XCode >= 3/28/11) XCodes */
/* #include "cpl_config_extras.h" */
