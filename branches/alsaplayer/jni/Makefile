
SRC =	flac/shndec.c  flac/main.c  flac/decoder.c  buffer.c alsa.c main.c linux_main.c  \
	ape/entropy.c  ape/filter-pre.c  ape/parser.c   ape/decoder.c  ape/main.c  ape/predictor.c


CFLAGS = -g -Wall -Wno-unused-variable -Wno-unused-function -Wno-implicit-function-declaration -DBUILD_STANDALONE -finline-functions -I. -I..

all = flacmacplay

all: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o flacmacplay -lpthread 


