C_SOURCES := main.c ordinary.c
MAKE_DSYM := NO

libmember.a: member.c
	$(CC) $(CFLAGS) -c $< -o member.o
	$(AR) $(ARFLAGS) $@ member.o

include Makefile.rules

OBJECTS += libmember.a
$(EXE): libmember.a
.DEFAULT_GOAL := $(EXE)
