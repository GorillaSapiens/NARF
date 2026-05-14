all:
	(cd src ; make)

tar:
	rm -f ../`basename $$(git rev-parse --show-toplevel)`.*.tar.gz
	git ls-files | tar -czv -T - -f ../`basename $$(git rev-parse --show-toplevel)`.`date "+%Y%m%d_%H%M%S"`.tar.gz
