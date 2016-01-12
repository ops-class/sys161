#ifndef _ARRAY_H_
#define _ARRAY_H_

/*
 * Resizeable array of void pointers.
 *
 * Functions:
 *     array_create  - allocate a new array object.
 *     array_getnum  - return the size of the array.
 *     array_getguy  - return the specified element from the array.
 *     array_setsize - alter the size of the array. If shrunk, excess 
 *                     elements are dropped. If expanded, the new elements
 *                     are *not* initialized.
 *     array_add     - add one pointer to the end of the array, increasing 
 *                     the size.
 *     array_remove  - remove one pointer from anywhere in the array. The
 *                     portion of the array after the index is moved down,
 *                     collapsing the resulting hole.
 *     array_destroy - dispose of an array. If not empty, the contents are
 *                     lost.
 */

struct array;  /* Opaque. */

struct array *array_create(void);
int           array_getnum(struct array *);
void         *array_getguy(struct array *, int index);
int           array_setsize(struct array *, int nguys);
int           array_add(struct array *, void *guy);
void          array_remove(struct array *, int index);
void          array_destroy(struct array *);

#endif /* _ARRAY_H_ */
