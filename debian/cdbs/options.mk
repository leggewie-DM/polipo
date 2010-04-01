common-install-arch::
	shopt nullglob; \
	install --mode=644 -D debian/options $(DEB_DESTDIR)/etc/polipo/
	mkdir -p $(DEB_DESTDIR)/var/cache/polipo
	chown proxy:proxy $(DEB_DESTDIR)/var/cache/polipo
	chmod 755 $(DEB_DESTDIR)/var/cache/polipo
	mkdir -p $(DEB_DESTDIR)/var/log/polipo
	chown proxy:adm $(DEB_DESTDIR)/var/log/polipo
	chmod 2755 $(DEB_DESTDIR)/var/log/polipo
