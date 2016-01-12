/*
 * Array of void pointers. See array.h.
 */
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "array.h"

struct array {
	void **v;
	int num;
	int max;
};

struct array *
array_create(void)
{
	struct array *a = malloc(sizeof(struct array));
	if (a==NULL) {
		return NULL;
	}
	a->v = NULL;
	a->num = 0;
	a->max = 0;
	return a;
}

int
array_getnum(struct array *a)
{
	return a->num;
}

void *
array_getguy(struct array *a, int index)
{
	assert(index >=0 && index < a->num);
	return a->v[index];
}

int
array_setsize(struct array *a, int nguys)
{
	if (nguys > a->max) {
		void **newv;
		int i;
		
		while (nguys > a->max) a->max = (a->max+1)*2;
		newv = malloc(a->max * sizeof(void *));
		if (newv==NULL) {
			return -1;
		}
		for (i=0; i<a->num; i++) newv[i] = a->v[i];
		if (a->v!=NULL) {
			free(a->v);
		}
		a->v = newv;
	}
	else if (nguys==0 && a->max > 16) {
		assert(a->v!=NULL);
		free(a->v);
		a->v = NULL;
		a->max = 0;
	}
	a->num = nguys;
	return 0;
}

int
array_add(struct array *a, void *guy)
{
	int ix = a->num;
	if (array_setsize(a, ix+1)) {
		return -1;
	}
	a->v[ix] = guy;
	return 0;
}

void
array_remove(struct array *a, int index)
{
	int nmove = a->num - (index + 1);
	memmove(a->v+index, a->v+index+1, nmove*sizeof(void *));
	a->num--;
}

void
array_destroy(struct array *a)
{
	if (a->v) free(a->v);
	free(a);
}
