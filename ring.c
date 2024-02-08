#include <unistd.h>

#include "ring.h"

int put_buf(struct mini_ring *ring, char *buf, size_t size)
{
	if(ring->head == ring->tail + MAX_NODE)
		return -1;

	ring->nodes[ring->head % MAX_NODE].size = size;
	ring->nodes[ring->head % MAX_NODE].ptr = buf;
	asm volatile("" ::: "memory");
	ring->head++;
	return 0;
}

int put_buf_block(struct mini_ring *ring, char *buf, size_t size)
{
	while(put_buf(ring, buf, size) != 0)
	{
		usleep(1000);
	}
}

int get_buf(struct mini_ring *ring, char **buf, size_t *size)
{
	if(ring->tail == ring->head)
		return -1;

	*size = ring->nodes[ring->tail % MAX_NODE].size;
	*buf = ring->nodes[ring->tail % MAX_NODE].ptr;
	asm volatile("" ::: "memory");
	ring->tail++;
	return 0;
}

int get_buf_block(struct mini_ring *ring, char **buf, size_t *size)
{
	while(get_buf(ring, buf, size) != 0)
	{
		usleep(1000);
	}
}