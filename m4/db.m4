# BEGIN COPYRIGHT BLOCK
# Copyright (C) 2024 Red Hat, Inc.
# All rights reserved.
#
# License: GPL (version 3 or any later version).
# See LICENSE for details. 
# END COPYRIGHT BLOCK

dnl ================================================================
dnl Berkeley DB backend configuration.
dnl
dnl BDB is disabled by default.  To enable it, pass one of:
dnl   --with-bdb              use system libdb
dnl   --with-bundle-libdb=DIR use a pre-built bundled libdb
dnl   --with-libbdb-ro        use the read-only BDB library (librobdb)
dnl
dnl LMDB is always built regardless of BDB settings.
dnl
dnl To remove BDB from the build system entirely, delete this file
dnl and the m4_include(m4/db.m4) line in configure.ac.
dnl ================================================================

dnl ------------------------------------------------------------------
dnl 1. Parse the three BDB knobs.
dnl ------------------------------------------------------------------

dnl -- --with-bdb (system libdb) --
AC_MSG_CHECKING(for --with-bdb)
AC_ARG_WITH(bdb,
  AS_HELP_STRING([--with-bdb],
                 [Enable Berkeley DB backend support (disabled by default)]),
[
  if test "$withval" = "no"; then
    with_bdb=no
    AC_MSG_RESULT(no)
  else
    with_bdb=yes
    AC_MSG_RESULT(yes)
  fi
],
[
  with_bdb=no
  AC_MSG_RESULT([no (default)])
])

dnl -- --with-bundle-libdb=DIR --
dblib=".libs/libdb-5.3-389ds.so"
AC_MSG_CHECKING(for --with-bundle-libdb)
AC_ARG_WITH([bundle-libdb],
  AS_HELP_STRING([--with-bundle-libdb=PATH],
                 [Directory containing $dblib and db.h (if not using system libdb package)])
)
if test -n "$with_bundle_libdb"; then
  if test "$with_bundle_libdb" = no ; then
    with_bundle_libdb=no
  elif ! test -f "$with_bundle_libdb/db.h" ; then
    AC_MSG_ERROR([Directory specified with --with-bundle-libdb=fullpath should contain db.h])
  elif ! test -f "$with_bundle_libdb/$dblib" ; then
    AC_MSG_ERROR([Directory specified with --with-bundle-libdb=fullpath should contain $dblib])
  else
    AC_MSG_RESULT([$with_bundle_libdb])
  fi
else
  with_bundle_libdb=no
fi
AM_CONDITIONAL([BUNDLE_LIBDB],[test "$with_bundle_libdb" != no])

dnl -- --with-libbdb-ro --
AC_MSG_CHECKING(for --with-libbdb-ro)
AC_ARG_WITH(libbdb-ro,
  AS_HELP_STRING([--with-libbdb-ro],
                 [Use a read-only Berkeley Database shared library (implies BDB support)]),
[
  if test "$withval" = "yes"; then
    with_libbdb_ro=yes
    AC_MSG_RESULT(yes)
  else
    with_libbdb_ro=no
    AC_MSG_RESULT(no)
  fi
],
[
  with_libbdb_ro=no
  AC_MSG_RESULT([no (default)])
])

dnl ------------------------------------------------------------------
dnl 2. Determine whether BDB is enabled.
dnl    Any of the three knobs turns it on.
dnl ------------------------------------------------------------------
if test "$with_bdb" = yes \
     -o "$with_bundle_libdb" != no \
     -o "$with_libbdb_ro" = yes; then
  without_bdb=no
else
  without_bdb=yes
fi

AM_CONDITIONAL([WITHOUT_BDB], [test "$without_bdb" = yes])
AM_CONDITIONAL([WITH_LIBBDB_RO],
               [test "$with_libbdb_ro" = yes -a "$without_bdb" != yes])

if test "$without_bdb" = yes; then
  dnl ----------------------------------------------------------------
  dnl 3a. BDB disabled – set empty variables, define WITHOUT_BDB.
  dnl ----------------------------------------------------------------
  AC_DEFINE([WITHOUT_BDB], [1], [Define if BDB support is completely disabled])
  AC_MSG_NOTICE([BDB backend support is disabled])

  db_bdb_srcdir="ldap/servers/slapd/back-ldbm/db-bdb"
  db_inc=""
  db_incdir=""
  db_lib=""
  db_libdir=""
  db_bindir="/usr/bin"
  db_libver=""

  AC_SUBST(db_bdb_srcdir)
  AC_SUBST(db_inc)
  AC_SUBST(db_incdir)
  AC_SUBST(db_lib)
  AC_SUBST(db_libdir)
  AC_SUBST(db_bindir)
  AC_SUBST(db_libver)

else
  dnl ----------------------------------------------------------------
  dnl 3b. BDB enabled – detect the library.
  dnl ----------------------------------------------------------------
  AC_MSG_NOTICE([BDB backend support is enabled])

  if test "$with_bundle_libdb" != no; then
    dnl ==============================================================
    dnl 3b-i.  Bundled libdb (--with-bundle-libdb=DIR)
    dnl ==============================================================
    AC_MSG_CHECKING(Handling bundle_libdb)

    db_lib="-L${with_bundle_libdb}/.libs -R${prefix}/lib64/dirsrv"
    db_incdir=$with_bundle_libdb
    db_inc="-I $db_incdir"
    db_libver="5.3-389ds"

    db_ver_maj=`grep DB_VERSION_MAJOR $db_incdir/db.h | awk '{print $3}'`
    db_ver_min=`grep DB_VERSION_MINOR $db_incdir/db.h | awk '{print $3}'`
    db_ver_pat=`grep DB_VERSION_PATCH $db_incdir/db.h | awk '{print $3}'`

    if test ${db_ver_maj} -lt 4; then
      AC_MSG_ERROR([Found db ${db_ver_maj}.${db_ver_min} is too old, update to version 4.7 at least])
    elif test ${db_ver_maj} -eq 4 -a ${db_ver_min} -lt 7; then
      AC_MSG_ERROR([Found db ${db_ver_maj}.${db_ver_min} is too old, update to version 4.7 at least])
    else
      AC_MSG_RESULT([libdb-${db_ver_maj}.${db_ver_min}-389ds.so])
    fi

    db_bdb_srcdir="ldap/servers/slapd/back-ldbm/db-bdb"

    AC_SUBST(db_bdb_srcdir)
    AC_SUBST(db_inc)
    AC_SUBST(db_lib)
    AC_SUBST(db_libver)

  else
    dnl ==============================================================
    dnl 3b-ii.  System libdb or read-only librobdb
    dnl         (--with-bdb or --with-libbdb-ro)
    dnl ==============================================================
    AC_MSG_CHECKING(for db)

    dnl -- optional path overrides --
    AC_MSG_CHECKING(for --with-db)
    AC_ARG_WITH(db,
      AS_HELP_STRING([--with-db@<:@=PATH@:>@],[Berkeley DB directory]),
    [
      if test "$withval" = "yes"; then
        AC_MSG_RESULT(yes)
      elif test "$withval" = "no"; then
        AC_MSG_RESULT(no)
        AC_MSG_ERROR([db is required.])
      elif test -d "$withval"/include -a -d "$withval"/lib; then
        AC_MSG_RESULT([using $withval])
        DBDIR=$withval
        db_lib="-L$DBDIR/lib"
        db_libdir="$DBDIR/lib"
        db_incdir="$DBDIR/include"
        if ! test -e "$db_incdir/db.h" ; then
          AC_MSG_ERROR([$withval include dir not found])
        fi
        db_inc="-I$db_incdir"
      else
        echo
        AC_MSG_ERROR([$withval not found])
      fi
    ],
    AC_MSG_RESULT(yes))

    AC_MSG_CHECKING(for --with-db-inc)
    AC_ARG_WITH(db-inc,
      AS_HELP_STRING([--with-db-inc=PATH],[Berkeley DB include file directory]),
    [
      if test -e "$withval"/db.h; then
        AC_MSG_RESULT([using $withval])
        db_incdir="$withval"
        db_inc="-I$withval"
      else
        echo
        AC_MSG_ERROR([$withval not found])
      fi
    ],
    AC_MSG_RESULT(no))

    AC_MSG_CHECKING(for --with-db-lib)
    AC_ARG_WITH(db-lib,
      AS_HELP_STRING([--with-db-lib=PATH],[Berkeley DB library directory]),
    [
      if test -d "$withval"; then
        AC_MSG_RESULT([using $withval])
        db_lib="-L$withval"
        db_libdir="$withval"
      else
        echo
        AC_MSG_ERROR([$withval not found])
      fi
    ],
    AC_MSG_RESULT(no))

    dnl -- locate db.h --
    db_bdb_srcdir="ldap/servers/slapd/back-ldbm/db-bdb"
    if test -z "$db_inc"; then
      AC_MSG_CHECKING(for db.h)
      if test "$with_libbdb_ro" = yes; then
        AC_MSG_RESULT([using lib/librobdb/lib/robdb.h])
        db_incdir="lib/librobdb/lib"
        db_inc="-Ilib/librobdb/lib"
        db_libdir=""
        db_lib="-lrobdb"
      elif test -f "/usr/include/db4/db.h"; then
        AC_MSG_RESULT([using /usr/include/db4/db.h])
        db_incdir="/usr/include/db4"
        db_inc="-I/usr/include/db4"
        db_lib='-L$(libdir)'
        db_libdir='$(libdir)'
      elif test -f "/usr/include/libdb/db.h"; then
        AC_MSG_RESULT([using /usr/include/libdb/db.h])
        db_incdir="/usr/include/libdb"
        db_inc="-I/usr/include/libdb"
        db_lib='-L$(libdir)'
        db_libdir='$(libdir)'
      elif test -f "/usr/include/db.h"; then
        AC_MSG_RESULT([using /usr/include/db.h])
        db_incdir="/usr/include"
        db_inc="-I/usr/include"
        db_lib='-L$(libdir)'
        db_libdir='$(libdir)'
      else
        AC_MSG_RESULT(no)
        AC_MSG_ERROR([db not found, specify with --with-db.])
      fi
    fi

    dnl -- figure out the version --
    if test "$with_libbdb_ro" = yes; then
      db_ver_maj=5
      db_ver_min=3
      db_ver_pat=0
    else
      db_ver_maj=`grep DB_VERSION_MAJOR $db_incdir/db.h | awk '{print $3}'`
      db_ver_min=`grep DB_VERSION_MINOR $db_incdir/db.h | awk '{print $3}'`
      db_ver_pat=`grep DB_VERSION_PATCH $db_incdir/db.h | awk '{print $3}'`
    fi

    if test ${db_ver_maj} -lt 4; then
      AC_MSG_ERROR([Found db ${db_ver_maj}.${db_ver_min} is too old, update to version 4.7 at least])
    elif test ${db_ver_maj} -eq 4 -a ${db_ver_min} -lt 7; then
      AC_MSG_ERROR([Found db ${db_ver_maj}.${db_ver_min} is too old, update to version 4.7 at least])
    fi

    db_libver=${db_ver_maj}.${db_ver_min}

    dnl -- verify the library exists (skip for read-only BDB) --
    if test "$with_libbdb_ro" != yes; then
      save_ldflags="$LDFLAGS"
      LDFLAGS="$db_lib $LDFLAGS"
      AC_CHECK_LIB([db-$db_libver], [db_create], [true],
        [AC_MSG_ERROR([$db_incdir/db.h is version $db_libver but libdb-$db_libver not found])],
        [$LIBNSL])
      LDFLAGS="$save_ldflags"
    fi

    dnl -- db_bindir via pkg-config --
    if test -n "$PKG_CONFIG"; then
      if $PKG_CONFIG --exists db; then
        db_bindir=`$PKG_CONFIG --variable=bindir db`
      else
        db_bindir=/usr/bin
      fi
    else
      db_bindir=/usr/bin
    fi

    AC_SUBST(db_bdb_srcdir)
    AC_SUBST(db_bdbro_srcdir)
    AC_SUBST(db_inc)
    AC_SUBST(db_incdir)
    AC_SUBST(db_lib)
    AC_SUBST(db_libdir)
    AC_SUBST(db_bindir)
    AC_SUBST(db_libver)

  fi
dnl end: BDB enabled
fi
