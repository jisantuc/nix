all-local: unpack.sh

install-exec-local:
	$(INSTALL) -d $(DESTDIR)$(datadir)/nix/corepkgs
	$(INSTALL) -d $(DESTDIR)$(datadir)/nix/corepkgs/channels
	$(INSTALL_DATA) $(srcdir)/unpack.nix $(DESTDIR)$(datadir)/nix/corepkgs/channels
	$(INSTALL_PROGRAM) unpack.sh $(DESTDIR)$(datadir)/nix/corepkgs/channels

include ../../substitute.mk

EXTRA_DIST = unpack.nix unpack.sh.in
