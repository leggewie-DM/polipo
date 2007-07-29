common-install-arch::
	shopt nullglob; \
	install --mode=644 -D debian/options $(DEB_DESTDIR)/etc/polipo/
