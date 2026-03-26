CFLAGS = -gdwarf-4 -g -static

.PHONY: all clean

all: $(patsubst %.c,%,$(wildcard *.c))

%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(patsubst %.c,%,$(wildcard *.c))
