# libguestfs virt-sparsify tool
# Copyright (C) 2011 Red Hat Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

include $(top_srcdir)/subdir-rules.mk

SOURCES =

EXTRA_DIST = \
	$(SOURCES) \
	virt-sparsify.pod \
	test-virt-sparsify.sh

CLEANFILES = *~ *.cmi *.cmo *.cmx *.cmxa *.o virt-sparsify test.img

if HAVE_OCAML

# Alphabetical order.
SOURCES += \
	progress_c.c \
	progress.mli \
	progress.ml \
	sparsify.ml \
	utils.ml

# Note this list must be in dependency order.
OBJECTS = \
	../fish/guestfish-progress.o \
	progress_c.o \
	utils.cmx \
	progress.cmx \
	sparsify.cmx

bin_SCRIPTS = virt-sparsify

# -I $(top_builddir)/src/.libs is a hack which forces corresponding -L
# option to be passed to gcc, so we don't try linking against an
# installed copy of libguestfs.
OCAMLPACKAGES = -package unix -I $(top_builddir)/src/.libs -I ../ocaml

OCAMLCFLAGS = -g -warn-error CDEFLMPSUVYZX $(OCAMLPACKAGES)
OCAMLOPTFLAGS = $(OCAMLCFLAGS)

virt-sparsify: $(OBJECTS)
	$(OCAMLFIND) ocamlopt $(OCAMLOPTFLAGS) \
	  mlguestfs.cmxa -linkpkg $^ -cclib -lncurses -o $@

.mli.cmi:
	$(OCAMLFIND) ocamlc $(OCAMLCFLAGS) -c $< -o $@
.ml.cmo:
	$(OCAMLFIND) ocamlc $(OCAMLCFLAGS) -c $< -o $@
.ml.cmx:
	$(OCAMLFIND) ocamlopt $(OCAMLCFLAGS) -c $< -o $@

# automake will decide we don't need C support in this file.  Really
# we do, so we have to provide it ourselves.

DEFAULT_INCLUDES = -I. -I$(top_builddir) -I$(shell $(OCAMLC) -where) -I../fish

.c.o:
	$(CC) $(CFLAGS) $(PROF_CFLAGS) $(DEFAULT_INCLUDES) -c $< -o $@

# Manual pages and HTML files for the website.

man_MANS = virt-sparsify.1

noinst_DATA = $(top_builddir)/html/virt-sparsify.1.html

virt-sparsify.1 $(top_builddir)/html/virt-sparsify.1.html: stamp-virt-sparsify.pod

stamp-virt-sparsify.pod: virt-sparsify.pod
	$(top_builddir)/podwrapper.sh \
	  --man virt-sparsify.1 \
	  --html $(top_builddir)/html/virt-sparsify.1.html \
	  $<
	touch $@

CLEANFILES += stamp-virt-sparsify.pod

# Tests.

random_val := $(shell awk 'BEGIN{srand(); print 1+int(255*rand())}' < /dev/null)

TESTS_ENVIRONMENT = \
	MALLOC_PERTURB_=$(random_val) \
	LD_LIBRARY_PATH=$(top_builddir)/src/.libs \
	LIBGUESTFS_PATH=$(top_builddir)/appliance \
	TMPDIR=$(top_builddir)

TESTS = test-virt-sparsify.sh

# Dependencies.
depend: .depend

.depend: $(wildcard *.mli) $(wildcard *.ml)
	rm -f $@ $@-t
	$(OCAMLFIND) ocamldep -I ../ocaml $^ | \
	  $(SED) 's/ *$$//' | \
	  $(SED) -e :a -e '/ *\\$$/N; s/ *\\\n */ /; ta' | \
	  sort > $@-t
	mv $@-t $@

include .depend

endif

.PHONY: depend docs

# Parallel builds don't obey dependencies for some reason we
# don't understand.
.NOTPARALLEL:
