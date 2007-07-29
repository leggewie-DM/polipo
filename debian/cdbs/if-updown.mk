common-install-arch::
	shopt nullglob; \
	for d in if-up if-down; do \
		for script in debian/updown/$$d/*; do \
			S=`basename $$script` ;\
			install --mode=755 -D $$script \
				$(DEB_DESTDIR)/etc/network/$$d.d/$$S ;\
		done ;\
	done
