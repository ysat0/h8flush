#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([h8flash], [2.00], [ysato@users.sourceforge.jp])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([usb.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB(usb, usb_bulk_write,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_bulk_read,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_init,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_get_busses,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_find_devices,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_get_busses,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_open,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_strerr,has_usb=1,has_usb=0)
AC_CHECK_LIB(usb, usb_claim_interface,has_usb=1,has_usb=0)
AC_CHECK_LIB(elf, elf_version,has_elf=1,has_elf=0)
AC_CHECK_LIB(elf, elf_begin,has_elf=1,has_elf=0)
AC_CHECK_LIB(elf, elf_kind,has_elf=1,has_elf=0)
AC_CHECK_LIB(elf, elf_getphdrnum,has_elf=1,has_elf=0)
AC_CHECK_LIB(elf, gelf_getphdr,has_elf=1,has_elf=0)
AC_CHECK_LIB(elf, elf_end,has_elf=1,has_elf=0)
if test $has_usb = 0; then
   AC_MSG_WARN("WARNING: can not found libusb.")
   AC_MSG_WARN("disabled usb functions');
else
   LIBS="-lusb"
fi
if test $has_elf = 0; then
   AC_MSG_WARN("WARNING: can not found libelf.")
   AC_MSG_WARN("disabled elf handle.');
else
   LIBS="$LIBS -lelf"
fi
# Checks for header files.
AC_CHECK_HEADERS([fcntl.h stddef.h stdlib.h string.h sys/time.h termios.h unistd.h usb.h gelf.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T

# Checks for library functions.
AC_CHECK_FUNCS([ftruncate memchr memset munmap select strncasecmp strtoul malloc mmap])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
