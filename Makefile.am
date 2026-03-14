bin_PROGRAMS = diffdup

diffdup_SOURCES = src/lib.c src/diffdup.c src/main.c

bin_SCRIPTS =

man_MANS = 

docdir = $(datadir)/doc/@PACKAGE@
doc_DATA =

exampledir = $(datarootdir)/doc/@PACKAGE@
example_DATA =

install-exec-hook:
	chmod 755 "$(DESTDIR)$(bindir)/diffdup$(EXEEXT)"
