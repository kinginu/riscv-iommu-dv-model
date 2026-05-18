SUBDIRS = libiommu libtables test
.PHONY: run lib all
all: lib
lib:
	@for i in libiommu libtables; do \
	    echo "Building $$i";\
	    (cd $$i; $(MAKE) -s --no-print-directory clean);\
	    (cd $$i; $(MAKE) -s --no-print-directory lib); done
testbench:
	@for i in $(SUBDIRS); do \
	    echo "Building $$i";\
	    (cd $$i; $(MAKE) -s --no-print-directory clean);\
	    (cd $$i; $(MAKE) -s --no-print-directory); done
clean:
	@for i in $(SUBDIRS); do \
	    (cd $$i; $(MAKE) -s --no-print-directory clean); done

run:
	./runtests
