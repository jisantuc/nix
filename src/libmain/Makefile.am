pkglib_LTLIBRARIES = libmain.la

libmain_la_SOURCES = shared.cc

libmain_la_LIBADD = ../libstore/libstore.la @boehmgc_lib@

pkginclude_HEADERS = shared.hh

AM_CXXFLAGS = \
 -DNIX_STORE_DIR=\"$(storedir)\" \
 -DNIX_DATA_DIR=\"$(datadir)\" \
 -DNIX_STATE_DIR=\"$(localstatedir)/nix\" \
 -DNIX_LOG_DIR=\"$(localstatedir)/log/nix\" \
 -DNIX_CONF_DIR=\"$(sysconfdir)/nix\" \
 -DNIX_LIBEXEC_DIR=\"$(libexecdir)\" \
 -DNIX_BIN_DIR=\"$(bindir)\" \
 -DNIX_VERSION=\"$(VERSION)\" \
 -I$(srcdir)/.. -I$(srcdir)/../libutil \
 -I$(srcdir)/../libstore
