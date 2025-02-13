CFLAGS=-O1 -Wall -Werror -Wimplicit-fallthrough
SRCS=$(wildcard src/*.c)
HDRS=$(wildcard src/*.h)
OBJS=$(patsubst src/%.c, obj/%.o, $(SRCS))
CC=clang

rvemu: $(OBJS)
	$(CC) -g $(CFLAGS)  -lm -o $@ $^ $(LDFLAGS)

$(OBJS): obj/%.o: src/%.c $(HDRS)
	@mkdir -p $$(dirname $@)
	$(CC) -g $(CFLAGS) -c -o $@ $<

clean:
	rm -rf rvemu obj/
sweep:
	rm -f TB_source/*.txt
	rm -f TB_trace/*.txt
	rm -f cachelog*
	rm -f log/*.txt


.PHONY: clean
