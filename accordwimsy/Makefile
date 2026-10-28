# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   make        accordwimsy, linked with libcurl
#   make test   the tests: no network (a fake GitHub and Forgejo), git on local bare repositories
#
# BSD make and GNU make alike. cJSON is ReiWimsy's copy (../rei/third_party/cjson).

CC ?= cc
CFLAGS ?= -O2 -g
CURL_CFLAGS ?= `pkg-config --cflags libcurl`
CURL_LIBS ?= `pkg-config --libs libcurl`
CJSON = ../rei/third_party/cjson
WARN = -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
DEFS = -D_POSIX_C_SOURCE=200809L
OURS = $(WARN) $(DEFS) -Isrc -I$(CJSON)

LOGIC = src/util.o src/config.o src/git.o src/sync.o src/cjson.o

all: accordwimsy

accordwimsy: $(LOGIC) src/http.o src/main.o
	$(CC) $(CFLAGS) -o $@ $(LOGIC) src/http.o src/main.o $(CURL_LIBS) -lm

test: accordwimsy tests/test_aw
	./tests/test_aw

tests/test_aw: $(LOGIC) tests/test_aw.c
	$(CC) $(CFLAGS) $(OURS) -o $@ tests/test_aw.c $(LOGIC) -lm

src/util.o: src/util.c src/aw.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/util.c
src/config.o: src/config.c src/config.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/config.c
src/git.o: src/git.c src/git.h src/config.h src/aw.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/git.c
src/sync.o: src/sync.c src/sync.h src/git.h src/config.h src/aw.h
	$(CC) $(CFLAGS) $(OURS) -c -o $@ src/sync.c
src/http.o: src/http.c src/http.h src/aw.h
	$(CC) $(CFLAGS) $(OURS) $(CURL_CFLAGS) -c -o $@ src/http.c
src/main.o: src/main.c src/sync.h src/http.h src/config.h src/aw.h
	$(CC) $(CFLAGS) $(OURS) $(CURL_CFLAGS) -c -o $@ src/main.c
# cJSON as its project builds it
src/cjson.o: $(CJSON)/cJSON.c $(CJSON)/cJSON.h
	$(CC) $(CFLAGS) -std=c89 -c -o $@ $(CJSON)/cJSON.c

clean:
	rm -f $(LOGIC) src/http.o src/main.o accordwimsy tests/test_aw

.PHONY: all test clean
