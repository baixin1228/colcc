#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_NODE 64

struct ring_node
{
	size_t size;
	char *ptr;
};

struct mini_ring
{
	volatile uint64_t head;
	volatile uint64_t tail;
	void *priv;
	struct ring_node nodes[MAX_NODE];
};

int put_buf(struct mini_ring *ring, char *buf, size_t size);
struct ring_node *get_buf(struct mini_ring *ring);
