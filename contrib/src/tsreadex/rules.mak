# tsreadex

TSREADEX_GIT_HASH := ef0335480e3f7890909e78a0091b146cebcdcfe5
TSREADEX_URL := https://github.com/neneka/tsreadex/archive/$(TSREADEX_GIT_HASH).tar.gz

PKGS += tsreadex

$(TARBALLS)/tsreadex-$(TSREADEX_GIT_HASH).tar.gz:
	$(call download_pkg,$(TSREADEX_URL),tsreadex)

.sum-tsreadex: tsreadex-$(TSREADEX_GIT_HASH).tar.gz

tsreadex: tsreadex-$(TSREADEX_GIT_HASH).tar.gz .sum-tsreadex
	$(UNPACK)
	$(MOVE)

.tsreadex: tsreadex
	cd $< && $(CXX) $(CXXFLAGS) -std=c++14 -c aac.cpp huffman.cpp servicefilter.cpp util.cpp
	cd $< && $(AR) rcs libtsreadex.a aac.o huffman.o servicefilter.o util.o
	cd $< && mkdir -p "$(PREFIX)/include/tsreadex"
	cd $< && cp *.hpp "$(PREFIX)/include/tsreadex/"
	cd $< && mkdir -p "$(PREFIX)/lib"
	cd $< && install -m 644 libtsreadex.a "$(PREFIX)/lib/"
	touch $@
