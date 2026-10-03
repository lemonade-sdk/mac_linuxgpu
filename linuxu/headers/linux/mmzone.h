/* linuxu: SHIM (third_party/linux/include/linux/mmzone.h) */
#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#include <linux/gfp.h>

#define NUMA_NO_NODE	(-1)

enum zone_type {
	ZONE_DMA,
	ZONE_NORMAL,
	ZONE_HIGHMEM,
	ZONE_DMA32,
	__MAX_ZONE,
};

/* minimal node_stat_item subset (values match vendor/mmzone.h for the
 * ttm-used entries; mod_lruvec_page_state is a no-op in the shim) */
enum node_stat_item {
	NR_FREE_PAGES = 0,
	NR_INACTIVE_FILE = 5,
	NR_INACTIVE_ANON,
	NR_ACTIVE_FILE,
	NR_ACTIVE_ANON,
	NR_GPU_ACTIVE,	/* Pages assigned to GPU objects */
	NR_GPU_RECLAIM,	/* Pages in shrinkable GPU pools */
	NR_ANON_MAPPED,
	NR_FILE_MAPPED,
	NR_UNSTABLE_ANON,
	NR_SLAB_RECLAIMABLE_B,
	NR_SLAB_UNRECLAIMABLE_B,
	NR_ISO_PAGES,
	NR_VMEVENT_PAGEFAULTS_MAJOR,
	NRPAGESTATS,
};

#define MAX_NUMA_NODES	1
#define MAX_NUMNODES	(MAX_NUMA_NODES)
#define MAX_NR_ZONES	__MAX_ZONE

/* ---- NUMA topology helpers (single-node model; kfd_crat.c / kfd_svm.c) ---- */
#define for_each_online_node(node) \
	for ((node) = 0; (node) < MAX_NUMNODES; (node)++)

#define for_each_node(node) \
	for ((node) = 0; (node) < MAX_NUMNODES; (node)++)

#ifndef _LINUXU_NUM_POSSIBLE_NODES
#define _LINUXU_NUM_POSSIBLE_NODES
static inline int num_possible_nodes(void)
{
	return MAX_NUMNODES;
}
#endif

static inline int node_distance(int from, int to)
{
	(void)from; (void)to;
	return 10; /* LOCAL_DISTANCE on a single-node system */
}

static inline int cpu_to_node(int cpu)
{
	(void)cpu;
	return 0;
}


#include <linux/cpumask.h>

struct zone {
	int zone_id;
	unsigned long managed_pages;
	struct pglist_data *zone_pgdat;
};
struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];
	long per_node_pages;
};
typedef struct pglist_data pg_data_t;
extern pg_data_t *linuxu_node_data(int nid);
static inline pg_data_t *NODE_DATA(int nid) { return linuxu_node_data(nid); }
static inline unsigned long zone_managed_pages(const struct zone *zone)
{ return zone ? zone->managed_pages : 0; }

static inline int num_online_nodes(void) { return 1; }
static inline bool node_online(int nid) { return nid == 0; }
static inline int first_online_node(void) { return 0; }
static inline int last_online_node(void) { return 0; }
static inline bool node_possible(int nid) { return nid == 0; }
static inline bool node_populated(int nid) { return nid == 0; }
static inline int default_numa_node(void) { return 0; }
static inline int numa_node_id(void) { return 0; }
/* for_each_node (single node) — see NUMA helpers above */
static inline void fs_reclaim_acquire(gfp_t gfp) { (void)gfp; }
static inline void fs_reclaim_release(gfp_t gfp) { (void)gfp; }
#ifndef _LINUXU_DEV_TO_NODE
#define _LINUXU_DEV_TO_NODE
static inline int dev_to_node(const struct device *dev) { (void)dev; return 0; }
#endif
static inline void mod_lruvec_page_state(struct page *p, enum node_stat_item item, int delta)
{	(void)p; (void)item; (void)delta; }

#endif /* _LINUX_MMZONE_H */

#ifndef MAX_PAGE_ORDER
#define MAX_PAGE_ORDER	10
#endif
#ifndef NR_PAGE_ORDERS
#define NR_PAGE_ORDERS	(MAX_PAGE_ORDER + 1)
#endif
