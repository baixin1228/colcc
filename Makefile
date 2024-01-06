colcc:$(subst .c,.o,$(wildcard *.c))
	$(CC) -O3 -o $@ $^ -luuid -lpthread `pkg-config --cflags --libs liblz4`

%.o:%.c
	$(CC) -O3 -g -c -o $@ $<

clean:
	rm -rf *.o colcc