#include "joold.h"

static struct joold_queue {
	int trash;
} throwaway;

struct joold_queue *joold_create(void)
{
	return &throwaway;
}
