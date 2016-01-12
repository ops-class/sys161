/*
 * The functions for accessing physical memory are inlined here
 * because they're called millions of times a second from the cpu
 * code.
 *
 * This file is logically part of bus/lamebus.c.
 *
 * The globals used by these functions (ram[] and bus_ramsize) are
 * declared in memdefs.h.
 */


/*
 * Fetch physical memory.
 */
static
inline
int
bus_mem_fetch(uint32_t offset, uint32_t *ret)
{
	char *ptr;
	
	if (offset >= bus_ramsize) {
		/* No such memory */
		return -1;
	}

	// If the pointer dereference below ever blows up, consider
	// uncommenting this for debugging.
	//Assert((offset & 0x3)==0);
	
	ptr = ram+offset;
	*ret = ntohl(*(uint32_t *)ptr);
	
	return 0;
}

/*
 * Fetch physical memory byte-wise.
 */
static
inline
int
bus_mem_fetchbyte(uint32_t offset, uint8_t *ret)
{
	char *ptr;
	
	if (offset >= bus_ramsize) {
		/* No such memory */
		return -1;
	}

	ptr = ram+offset;
	*ret = *(uint8_t *)ptr;
	
	return 0;
}

/*
 * Store to physical memory.
 */
static
inline
int
bus_mem_store(uint32_t offset, uint32_t val)
{
	char *ptr;

	if (offset >= bus_ramsize) {
		/* No such memory */
		return -1;
	}

	// If the pointer dereference below ever blows up, consider
	// uncommenting this for debugging.
	//Assert((offset & 0x3)==0);

	ptr = ram+offset;
	*(uint32_t *)ptr = htonl(val);
	
	return 0;
}

/*
 * Store to physical memory, byte-wise.
 */
static
inline
int
bus_mem_storebyte(uint32_t offset, uint8_t val)
{
	char *ptr;

	if (offset >= bus_ramsize) {
		/* No such memory */
		return -1;
	}

	ptr = ram+offset;
	*(uint8_t *)ptr = val;

	return 0;
}

/*
 * Get a (readonly) pointer to a page of physical memory.
 */
static
inline
const uint32_t *
bus_mem_map(uint32_t offset)
{
	if (offset >= bus_ramsize) {
		/* No such memory */
		return NULL;
	}
	return (uint32_t *)(ram+offset);
}

static
inline
uint32_t
bus_use_map(const uint32_t *page, uint32_t pageoffset)
{
	return ntohl(page[pageoffset/sizeof(uint32_t)]);
}
