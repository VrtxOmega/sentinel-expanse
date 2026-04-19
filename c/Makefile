all:
	gcc -O3 sentinel_v6.c -o sentinel_v6 -lz
	gcc -O3 sentinel_v7.c -o sentinel_v7 -lz

test: all
	cd .. && pytest tests/test_roundtrip.py -v

clean:
	rm -f sentinel_v6 sentinel_v7
