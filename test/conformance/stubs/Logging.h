#pragma once

// Host-side stub of lib/Logging for the conformance test: errors go to
// stderr so the dump tool's stdout stays pure JSON.

#include <cstdio>

#define LOG_ERR(tag, fmt, ...) fprintf(stderr, "[ERR] [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_INF(tag, fmt, ...) fprintf(stderr, "[INF] [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_DBG(tag, fmt, ...) fprintf(stderr, "[DBG] [%s] " fmt "\n", tag, ##__VA_ARGS__)
