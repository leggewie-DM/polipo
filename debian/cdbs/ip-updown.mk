common-install-arch::
	shopt nullglob; \
	for d in ip-up ip-down; do \
		for script in debian/$$d/*; do \
			S=`basename $$script` ;\
			install --mode=755 -D $$script \
				$(DEB_DESTDIR)/etc/ppp/$$d.d/$$S ;\
		done ;\
	done
