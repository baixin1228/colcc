colcc:$(subst .c,.o,$(wildcard *.c))
	$(CC) -O3 -o $@ $^ -luuid -lpthread

%.o:%.c
	$(CC) -O3 -g -c -o $@ $<

clean:
	rm -rf *.o colcc