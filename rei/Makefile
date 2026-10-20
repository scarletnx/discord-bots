# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   make [CONCORD=prefix]  reiwimsy, linked with Concord v3.0.1 as make install PREFIX=prefix put it, and libcurl
#   make test              the tests: no Discord, no network, no Concord
#
# BSD make and GNU make alike.

CC ?= cc
CFLAGS ?= -O2 -g
CONCORD ?= /usr/local
CURL_CFLAGS ?= `pkg-config --cflags libcurl`
CURL_LIBS ?= `pkg-config --libs libcurl`
WARN = -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
DEFS = -D_POSIX_C_SOURCE=200809L
OURS = $(WARN) $(DEFS) -Isrc -Ithird_party/cjson

LOGIC = src/util.o src/refs.o src/github.o src/look.o src/link.o src/cards.o src/state.o src/rest.o \
	third_party/cjson/cJSON.o

all: reiwimsy

reiwimsy: $(LOGIC) src/http.o src/main.o
	$(CC) $(CFLAGS) -o $@ $(LOGIC) src/http.o src/main.o $(CONCORD)/lib/libdiscord.a $(CURL_LIBS) -lpthread -lm

test: tests/test_rei
	./tests/test_rei

tests/test_rei: $(LOGIC) tests/test_rei.c
	$(CC) $(CFLAGS) $(OURS) -o $@ tests/test_rei.c $(LOGIC) -lm

src/util.o: src/util.c src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/util.c
src/refs.o: src/refs.c src/refs.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/refs.c
src/github.o: src/github.c src/github.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/github.c
src/look.o: src/look.c src/look.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/look.c
src/link.o: src/link.c src/link.h src/look.h src/github.h src/refs.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/link.c
src/cards.o: src/cards.c src/cards.h src/look.h src/github.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/cards.c
src/state.o: src/state.c src/state.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/state.c
src/rest.o: src/rest.c src/rest.h src/github.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/rest.c
src/http.o: src/http.c src/http.h src/github.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) $(CURL_CFLAGS) -c -o $@ src/http.c
# Concord's headers are not ours: their warnings are theirs
src/main.o: src/main.c src/cards.h src/http.h src/link.h src/rest.h src/state.h src/rei.h
	$(CC) $(CFLAGS) $(OURS) $(CURL_CFLAGS) -isystem $(CONCORD)/include/concord -c -o $@ src/main.c
# cJSON as its project builds it
third_party/cjson/cJSON.o: third_party/cjson/cJSON.c third_party/cjson/cJSON.h
	$(CC) $(CFLAGS) -std=c89 -c -o $@ third_party/cjson/cJSON.c

clean:
	rm -f $(LOGIC) src/http.o src/main.o reiwimsy tests/test_rei

.PHONY: all test clean
