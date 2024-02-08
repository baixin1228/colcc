colcc:$(subst .c,.o,$(wildcard *.c))
	$(CC) -O3 -o $@ $^ -lpthread `pkg-config --cflags --libs liblz4 uuid`

%.o:%.c
	$(CC) -O3 -g -c -o $@ $<

clean:
	rm -rf *.o *.i colcc main_1