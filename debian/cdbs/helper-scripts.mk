common-install-arch::
	shopt nullglob; \
	for p in debian/helper-scripts/*; do \
		PACKAGE=`basename $$p` ;\
		for script in $$p/*; do \
			S=`basename $$script` ;\
			install --mode=755 -D $$script \
				$(DEB_DESTDIR)/usr/lib/$$PACKAGE/$$S ;\
		done ;\
	done
