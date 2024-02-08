#include "ring.h"

int put_buf(struct mini_ring *ring, char *buf, size_t size)
{
	if(ring->head == ring->tail + MAX_NODE)
		return -1;

	ring->nodes[ring->head % MAX_NODE].size = size;
	ring->nodes[ring->head % MAX_NODE].ptr = buf;
	ring->head++;
	return 0;
}

struct ring_node *get_buf(struct mini_ring *ring)
{
	if(ring->tail == ring->head)
		return NULL;

	return &ring->nodes[ring->tail++ % MAX_NODE];
}