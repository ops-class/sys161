/*
 * Structures for the gmon.out file.
 *
 * We output the GNU format gmon.out, since it's vaguely documented in
 * the GNU gprof docs. The older BSD format doesn't appear to be
 * documented anywhere at all.
 *
 * The file begins with a gmon_file_header, which is then followed by
 * profiling records. A record is introduced by a one-byte record type
 * (GMON_RT_* below).
 *
 * The sample histogram is one record and consists of a gmon_histogram_header
 * followed by the correct number of gmon_histogram_entry structures.
 *
 * Each call graph edge is one record and consists of a single 
 * gmon_callgraph_entry.
 *
 * We don't generate basic block records.
 */

/* all our targets are 32-bit machines */
typedef uint32_t target_uintptr_t;

/* file header */
struct gmon_file_header {
	char gfh_magic[4];	/* "gmon" */
	uint32_t gfh_version;	/* GMON_VERSION */
	char gfh_unused[12];
};

#define GMON_VERSION  1

/* record types */
#define GMON_RT_HISTOGRAM  0
#define GMON_RT_CALLGRAPH  1
#define GMON_RT_BASICBLOCK 2

/* histogram header */
struct gmon_histogram_header {
	target_uintptr_t ghh_lowpc;
	target_uintptr_t ghh_highpc;
	uint32_t ghh_size;	/* size of data in u16's, not incl. header */
	uint32_t ghh_hz;	/* sample frequency */
	char ghh_name[15];
	char ghh_abbrev;
};

/* histogram entry */
struct gmon_histogram_entry {
	uint16_t ghe_count;
};

/* call graph record */
struct gmon_callgraph_entry {
	target_uintptr_t gce_from;
	target_uintptr_t gce_to;
	uint32_t gce_count;
};
