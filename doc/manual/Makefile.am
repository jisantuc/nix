DOCBOOK_DTD = /nix/current/xml/dtd/docbook
DOCBOOK_XSL = /nix/current/xml/xsl/docbook

ENV = SGML_CATALOG_FILES=$(DOCBOOK_DTD)/docbook.cat

XMLLINT = $(ENV) xmllint --catalogs
XSLTPROC = $(ENV) xsltproc --catalogs

SOURCES = book.xml introduction.xml installation.xml nix-reference.xml \
 troubleshooting.xml bugs.xml

book.is-valid: $(SOURCES)
	$(XMLLINT) --noout --valid book.xml
	touch $@

man1_MANS = nix.1 fix.1

man nix.1 fix.1: $(SOURCES) book.is-valid
	$(XSLTPROC) $(DOCBOOK_XSL)/manpages/docbook.xsl book.xml

book.html: $(SOURCES) book.is-valid
	$(XSLTPROC) --output book.html $(DOCBOOK_XSL)/html/docbook.xsl book.xml

all-local: book.html

install-data-local: book.html
	$(INSTALL) -d $(datadir)/nix/manual
	$(INSTALL_DATA) book.html $(datadir)/nix/manual

EXTRA_DIST = $(SOURCES) book.html nix.1 fix.1 book.is-valid