all:
	(cd src ; make)
	src/narf_details

clean:
	(cd src ; make clean)

tar:
	rm -f ../`basename $$(git rev-parse --show-toplevel)`.*.tar.gz
	git ls-files | tar -czv -T - -f /tmp/`basename $$(git rev-parse --show-toplevel)`.`date "+%Y%m%d_%H%M%S"`.tar.gz
