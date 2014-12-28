/*
 * sgen-gc.c: Simple generational GC.
 *
 * Author:
 * 	Paolo Molaro (lupus@ximian.com)
 *  Rodrigo Kumpera (kumpera@gmail.com)
 *
 * Copyright 2005-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 *
 * Thread start/stop adapted from Boehm's GC:
 * Copyright (c) 1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1996 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1998 by Fergus Henderson.  All rights reserved.
 * Copyright (c) 2000-2004 by Hewlett-Packard Company.  All rights reserved.
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin, Inc.
 * Copyright (C) 2012 Xamarin Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License 2.0 as published by the Free Software Foundation;
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License 2.0 along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Important: allocation provides always zeroed memory, having to do
 * a memset after allocation is deadly for performance.
 * Memory usage at startup is currently as follows:
 * 64 KB pinned space
 * 64 KB internal space
 * size of nursery
 * We should provide a small memory config with half the sizes
 *
 * We currently try to make as few mono assumptions as possible:
 * 1) 2-word header with no GC pointers in it (first vtable, second to store the
 *    forwarding ptr)
 * 2) gc descriptor is the second word in the vtable (first word in the class)
 * 3) 8 byte alignment is the minimum and enough (not true for special structures (SIMD), FIXME)
 * 4) there is a function to get an object's size and the number of
 *    elements in an array.
 * 5) we know the special way bounds are allocated for complex arrays
 * 6) we know about proxies and how to treat them when domains are unloaded
 *
 * Always try to keep stack usage to a minimum: no recursive behaviour
 * and no large stack allocs.
 *
 * General description.
 * Objects are initially allocated in a nursery using a fast bump-pointer technique.
 * When the nursery is full we start a nursery collection: this is performed with a
 * copying GC.
 * When the old generation is full we start a copying GC of the old generation as well:
 * this will be changed to mark&sweep with copying when fragmentation becomes to severe
 * in the future.  Maybe we'll even do both during the same collection like IMMIX.
 *
 * The things that complicate this description are:
 * *) pinned objects: we can't move them so we need to keep track of them
 * *) no precise info of the thread stacks and registers: we need to be able to
 *    quickly find the objects that may be referenced conservatively and pin them
 *    (this makes the first issues more important)
 * *) large objects are too expensive to be dealt with using copying GC: we handle them
 *    with mark/sweep during major collections
 * *) some objects need to not move even if they are small (interned strings, Type handles):
 *    we use mark/sweep for them, too: they are not allocated in the nursery, but inside
 *    PinnedChunks regions
 */

/*
 * TODO:

 *) we could have a function pointer in MonoClass to implement
  customized write barriers for value types

 *) investigate the stuff needed to advance a thread to a GC-safe
  point (single-stepping, read from unmapped memory etc) and implement it.
  This would enable us to inline allocations and write barriers, for example,
  or at least parts of them, like the write barrier checks.
  We may need this also for handling precise info on stacks, even simple things
  as having uninitialized data on the stack and having to wait for the prolog
  to zero it. Not an issue for the last frame that we scan conservatively.
  We could always not trust the value in the slots anyway.

 *) modify the jit to save info about references in stack locations:
  this can be done just for locals as a start, so that at least
  part of the stack is handled precisely.

 *) test/fix endianess issues

 *) Implement a card table as the write barrier instead of remembered
    sets?  Card tables are not easy to implement with our current
    memory layout.  We have several different kinds of major heap
    objects: Small objects in regular blocks, small objects in pinned
    chunks and LOS objects.  If we just have a pointer we have no way
    to tell which kind of object it points into, therefore we cannot
    know where its card table is.  The least we have to do to make
    this happen is to get rid of write barriers for indirect stores.
    (See next item)

 *) Get rid of write barriers for indirect stores.  We can do this by
    telling the GC to wbarrier-register an object once we do an ldloca
    or ldelema on it, and to unregister it once it's not used anymore
    (it can only travel downwards on the stack).  The problem with
    unregistering is that it needs to happen eventually no matter
    what, even if exceptions are thrown, the thread aborts, etc.
    Rodrigo suggested that we could do only the registering part and
    let the collector find out (pessimistically) when it's safe to
    unregister, namely when the stack pointer of the thread that
    registered the object is higher than it was when the registering
    happened.  This might make for a good first implementation to get
    some data on performance.

 *) Some sort of blacklist support?  Blacklists is a concept from the
    Boehm GC: if during a conservative scan we find pointers to an
    area which we might use as heap, we mark that area as unusable, so
    pointer retention by random pinning pointers is reduced.

 *) experiment with max small object size (very small right now - 2kb,
    because it's tied to the max freelist size)

  *) add an option to mmap the whole heap in one chunk: it makes for many
     simplifications in the checks (put the nursery at the top and just use a single
     check for inclusion/exclusion): the issue this has is that on 32 bit systems it's
     not flexible (too much of the address space may be used by default or we can't
     increase the heap as needed) and we'd need a race-free mechanism to return memory
     back to the system (mprotect(PROT_NONE) will still keep the memory allocated if it
     was written to, munmap is needed, but the following mmap may not find the same segment
     free...)

 *) memzero the major fragments after restarting the world and optionally a smaller
    chunk at a time

 *) investigate having fragment zeroing threads

 *) separate locks for finalization and other minor stuff to reduce
    lock contention

 *) try a different copying order to improve memory locality

 *) a thread abort after a store but before the write barrier will
    prevent the write barrier from executing

 *) specialized dynamically generated markers/copiers

 *) Dynamically adjust TLAB size to the number of threads.  If we have
    too many threads that do allocation, we might need smaller TLABs,
    and we might get better performance with larger TLABs if we only
    have a handful of threads.  We could sum up the space left in all
    assigned TLABs and if that's more than some percentage of the
    nursery size, reduce the TLAB size.

 *) Explore placing unreachable objects on unused nursery memory.
	Instead of memset'ng a region to zero, place an int[] covering it.
	A good place to start is add_nursery_frag. The tricky thing here is
	placing those objects atomically outside of a collection.

 *) Allocation should use asymmetric Dekker synchronization:
 	http://blogs.oracle.com/dave/resource/Asymmetric-Dekker-Synchronization.txt
	This should help weak consistency archs.
 */
#include "config.h"
#ifdef HAVE_SGEN_GC

#ifdef __MACH__
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#define _DARWIN_C_SOURCE
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#ifdef HAVE_PTHREAD_NP_H
#include <pthread_np.h>
#endif
#ifdef HAVE_SEMAPHORE_H
#include <semaphore.h>
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "metadata/sgen-gc.h"
#include "metadata/sgen-cardtable.h"
#include "metadata/sgen-protocol.h"
#include "metadata/sgen-archdep.h"
#include "metadata/sgen-bridge.h"
#include "metadata/sgen-memory-governor.h"
#include "metadata/sgen-hash-table.h"
#include "metadata/mono-gc.h"
#include "metadata/sgen-cardtable.h"
#include "metadata/sgen-pinning.h"
#include "metadata/sgen-workers.h"
#include "metadata/sgen-client.h"
#include "metadata/sgen-pointer-queue.h"
#include "utils/mono-mmap.h"
#include "utils/mono-time.h"
#include "utils/mono-semaphore.h"
#include "utils/mono-counters.h"
#include "utils/mono-proclib.h"
#include "utils/mono-memory-model.h"

#include <mono/utils/memcheck.h>

#if defined(__MACH__)
#include "utils/mach-support.h"
#endif

#define OPDEF(a,b,c,d,e,f,g,h,i,j) \
	a = i,

enum {
#include "mono/cil/opcode.def"
	CEE_LAST
};

#undef OPDEF

#undef pthread_create
#undef pthread_join
#undef pthread_detach

/*
 * ######################################################################
 * ########  Types and constants used by the GC.
 * ######################################################################
 */

/* 0 means not initialized, 1 is initialized, -1 means in progress */
static int gc_initialized = 0;
/* If set, check if we need to do something every X allocations */
gboolean has_per_allocation_action;
/* If set, do a heap check every X allocation */
guint32 verify_before_allocs = 0;
/* If set, do a minor collection before every X allocation */
guint32 collect_before_allocs = 0;
/* If set, do a whole heap check before each collection */
static gboolean whole_heap_check_before_collection = FALSE;
/* If set, do a heap consistency check before each minor collection */
static gboolean consistency_check_at_minor_collection = FALSE;
/* If set, do a mod union consistency check before each finishing collection pause */
static gboolean mod_union_consistency_check = FALSE;
/* If set, check whether mark bits are consistent after major collections */
static gboolean check_mark_bits_after_major_collection = FALSE;
/* If set, check that all nursery objects are pinned/not pinned, depending on context */
static gboolean check_nursery_objects_pinned = FALSE;
/* If set, do a few checks when the concurrent collector is used */
static gboolean do_concurrent_checks = FALSE;
/* If set, mark stacks conservatively, even if precise marking is possible */
static gboolean conservative_stack_mark = FALSE;
/* If set, do a plausibility check on the scan_starts before and after
   each collection */
static gboolean do_scan_starts_check = FALSE;

/*
 * If the major collector is concurrent and this is FALSE, we will
 * never initiate a synchronous major collection, unless requested via
 * GC.Collect().
 */
static gboolean allow_synchronous_major = TRUE;
static gboolean disable_minor_collections = FALSE;
static gboolean disable_major_collections = FALSE;
static gboolean do_verify_nursery = FALSE;
static gboolean do_dump_nursery_content = FALSE;
static gboolean enable_nursery_canaries = FALSE;

#ifdef HEAVY_STATISTICS
guint64 stat_objects_alloced_degraded = 0;
guint64 stat_bytes_alloced_degraded = 0;

guint64 stat_copy_object_called_nursery = 0;
guint64 stat_objects_copied_nursery = 0;
guint64 stat_copy_object_called_major = 0;
guint64 stat_objects_copied_major = 0;

guint64 stat_scan_object_called_nursery = 0;
guint64 stat_scan_object_called_major = 0;

guint64 stat_slots_allocated_in_vain;

guint64 stat_nursery_copy_object_failed_from_space = 0;
guint64 stat_nursery_copy_object_failed_forwarded = 0;
guint64 stat_nursery_copy_object_failed_pinned = 0;
guint64 stat_nursery_copy_object_failed_to_space = 0;

static int stat_wbarrier_add_to_global_remset = 0;
static int stat_wbarrier_set_field = 0;
static int stat_wbarrier_set_arrayref = 0;
static int stat_wbarrier_arrayref_copy = 0;
static int stat_wbarrier_generic_store = 0;
static int stat_wbarrier_generic_store_atomic = 0;
static int stat_wbarrier_set_root = 0;
static int stat_wbarrier_value_copy = 0;
static int stat_wbarrier_object_copy = 0;
#endif

static guint64 stat_pinned_objects = 0;

static guint64 time_minor_pre_collection_fragment_clear = 0;
static guint64 time_minor_pinning = 0;
static guint64 time_minor_scan_remsets = 0;
static guint64 time_minor_scan_pinned = 0;
static guint64 time_minor_scan_roots = 0;
static guint64 time_minor_finish_gray_stack = 0;
static guint64 time_minor_fragment_creation = 0;

static guint64 time_major_pre_collection_fragment_clear = 0;
static guint64 time_major_pinning = 0;
static guint64 time_major_scan_pinned = 0;
static guint64 time_major_scan_roots = 0;
static guint64 time_major_scan_mod_union = 0;
static guint64 time_major_finish_gray_stack = 0;
static guint64 time_major_free_bigobjs = 0;
static guint64 time_major_los_sweep = 0;
static guint64 time_major_sweep = 0;
static guint64 time_major_fragment_creation = 0;

static guint64 time_max = 0;

static SGEN_TV_DECLARE (time_major_conc_collection_start);
static SGEN_TV_DECLARE (time_major_conc_collection_end);

static SGEN_TV_DECLARE (last_minor_collection_start_tv);
static SGEN_TV_DECLARE (last_minor_collection_end_tv);

int gc_debug_level = 0;
FILE* gc_debug_file;

/*
void
mono_gc_flush_info (void)
{
	fflush (gc_debug_file);
}
*/

#define TV_DECLARE SGEN_TV_DECLARE
#define TV_GETTIME SGEN_TV_GETTIME
#define TV_ELAPSED SGEN_TV_ELAPSED

SGEN_TV_DECLARE (sgen_init_timestamp);

NurseryClearPolicy nursery_clear_policy = CLEAR_AT_TLAB_CREATION;

#define object_is_forwarded	SGEN_OBJECT_IS_FORWARDED
#define object_is_pinned	SGEN_OBJECT_IS_PINNED
#define pin_object		SGEN_PIN_OBJECT

#define ptr_in_nursery sgen_ptr_in_nursery

#define LOAD_VTABLE	SGEN_LOAD_VTABLE

gboolean
nursery_canaries_enabled (void)
{
	return enable_nursery_canaries;
}

#define safe_object_get_size	sgen_safe_object_get_size

/*
 * ######################################################################
 * ########  Global data.
 * ######################################################################
 */
LOCK_DECLARE (gc_mutex);
gboolean sgen_try_free_some_memory;

#define SCAN_START_SIZE	SGEN_SCAN_START_SIZE

static mword pagesize = 4096;
size_t degraded_mode = 0;

static mword bytes_pinned_from_failed_allocation = 0;

GCMemSection *nursery_section = NULL;
static volatile mword lowest_heap_address = ~(mword)0;
static volatile mword highest_heap_address = 0;

LOCK_DECLARE (sgen_interruption_mutex);

int current_collection_generation = -1;
volatile gboolean concurrent_collection_in_progress = FALSE;

/* objects that are ready to be finalized */
static SgenPointerQueue fin_ready_queue = SGEN_POINTER_QUEUE_INIT (INTERNAL_MEM_FINALIZE_READY);
static SgenPointerQueue critical_fin_queue = SGEN_POINTER_QUEUE_INIT (INTERNAL_MEM_FINALIZE_READY);

/* registered roots: the key to the hash is the root start address */
/* 
 * Different kinds of roots are kept separate to speed up pin_from_roots () for example.
 */
SgenHashTable roots_hash [ROOT_TYPE_NUM] = {
	SGEN_HASH_TABLE_INIT (INTERNAL_MEM_ROOTS_TABLE, INTERNAL_MEM_ROOT_RECORD, sizeof (RootRecord), mono_aligned_addr_hash, NULL),
	SGEN_HASH_TABLE_INIT (INTERNAL_MEM_ROOTS_TABLE, INTERNAL_MEM_ROOT_RECORD, sizeof (RootRecord), mono_aligned_addr_hash, NULL),
	SGEN_HASH_TABLE_INIT (INTERNAL_MEM_ROOTS_TABLE, INTERNAL_MEM_ROOT_RECORD, sizeof (RootRecord), mono_aligned_addr_hash, NULL)
};
static mword roots_size = 0; /* amount of memory in the root set */

MonoNativeTlsKey thread_info_key;

#ifdef HAVE_KW_THREAD
__thread SgenThreadInfo *sgen_thread_info;
__thread char *stack_end;
#endif

/* The size of a TLAB */
/* The bigger the value, the less often we have to go to the slow path to allocate a new 
 * one, but the more space is wasted by threads not allocating much memory.
 * FIXME: Tune this.
 * FIXME: Make this self-tuning for each thread.
 */
guint32 tlab_size = (1024 * 4);

#define MAX_SMALL_OBJ_SIZE	SGEN_MAX_SMALL_OBJ_SIZE

/* Functions supplied by the runtime to be called by the GC */
static MonoGCCallbacks gc_callbacks;

#define ALLOC_ALIGN		SGEN_ALLOC_ALIGN

#define ALIGN_UP		SGEN_ALIGN_UP

#ifdef SGEN_DEBUG_INTERNAL_ALLOC
MonoNativeThreadId main_gc_thread = NULL;
#endif

/*Object was pinned during the current collection*/
static mword objects_pinned;

/*
 * ######################################################################
 * ########  Macros and function declarations.
 * ######################################################################
 */

typedef SgenGrayQueue GrayQueue;

/* forward declarations */
static void scan_thread_data (void *start_nursery, void *end_nursery, gboolean precise, ScanCopyContext ctx);
static void scan_from_registered_roots (char *addr_start, char *addr_end, int root_type, ScanCopyContext ctx);

static void pin_from_roots (void *start_nursery, void *end_nursery, ScanCopyContext ctx);
static void finish_gray_stack (int generation, ScanCopyContext ctx);


SgenMajorCollector major_collector;
SgenMinorCollector sgen_minor_collector;
/* FIXME: get rid of this */
static GrayQueue gray_queue;

static SgenRememberedSet remset;

/* The gray queue to use from the main collection thread. */
#define WORKERS_DISTRIBUTE_GRAY_QUEUE	(&gray_queue)

/*
 * The gray queue a worker job must use.  If we're not parallel or
 * concurrent, we use the main gray queue.
 */
static SgenGrayQueue*
sgen_workers_get_job_gray_queue (WorkerData *worker_data)
{
	return worker_data ? &worker_data->private_gray_queue : WORKERS_DISTRIBUTE_GRAY_QUEUE;
}

static void
gray_queue_redirect (SgenGrayQueue *queue)
{
	gboolean wake = FALSE;

	for (;;) {
		GrayQueueSection *section = sgen_gray_object_dequeue_section (queue);
		if (!section)
			break;
		sgen_section_gray_queue_enqueue (queue->alloc_prepare_data, section);
		wake = TRUE;
	}

	if (wake) {
		g_assert (concurrent_collection_in_progress);
		sgen_workers_ensure_awake ();
	}
}

static void
gray_queue_enable_redirect (SgenGrayQueue *queue)
{
	if (!concurrent_collection_in_progress)
		return;

	sgen_gray_queue_set_alloc_prepare (queue, gray_queue_redirect, sgen_workers_get_distribute_section_gray_queue ());
	gray_queue_redirect (queue);
}

void
sgen_scan_area_with_callback (char *start, char *end, IterateObjectCallbackFunc callback, void *data, gboolean allow_flags)
{
	GCVTable *array_fill_vtable = sgen_client_get_array_fill_vtable ();

	while (start < end) {
		size_t size;
		char *obj;

		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}

		if (allow_flags) {
			if (!(obj = SGEN_OBJECT_IS_FORWARDED (start)))
				obj = start;
		} else {
			obj = start;
		}

		if ((GCVTable*)SGEN_LOAD_VTABLE (obj) != array_fill_vtable) {
			CHECK_CANARY_FOR_OBJECT (obj);
			size = ALIGN_UP (safe_object_get_size ((GCObject*)obj));
			callback (obj, size, data);
			CANARIFY_SIZE (size);
		} else {
			size = ALIGN_UP (safe_object_get_size ((GCObject*)obj));
		}

		start += size;
	}
}

/*
 * sgen_add_to_global_remset:
 *
 *   The global remset contains locations which point into newspace after
 * a minor collection. This can happen if the objects they point to are pinned.
 *
 * LOCKING: If called from a parallel collector, the global remset
 * lock must be held.  For serial collectors that is not necessary.
 */
void
sgen_add_to_global_remset (gpointer ptr, gpointer obj)
{
	SGEN_ASSERT (5, sgen_ptr_in_nursery (obj), "Target pointer of global remset must be in the nursery");

	HEAVY_STAT (++stat_wbarrier_add_to_global_remset);

	if (!major_collector.is_concurrent) {
		SGEN_ASSERT (5, current_collection_generation != -1, "Global remsets can only be added during collections");
	} else {
		if (current_collection_generation == -1)
			SGEN_ASSERT (5, sgen_concurrent_collection_in_progress (), "Global remsets outside of collection pauses can only be added by the concurrent collector");
	}

	if (!object_is_pinned (obj))
		SGEN_ASSERT (5, sgen_minor_collector.is_split || sgen_concurrent_collection_in_progress (), "Non-pinned objects can only remain in nursery if it is a split nursery");
	else if (sgen_cement_lookup_or_register (obj))
		return;

	remset.record_pointer (ptr);

	sgen_pin_stats_register_global_remset (obj);

	SGEN_LOG (8, "Adding global remset for %p", ptr);
	binary_protocol_global_remset (ptr, obj, (gpointer)SGEN_LOAD_VTABLE (obj));
}

/*
 * sgen_drain_gray_stack:
 *
 *   Scan objects in the gray stack until the stack is empty. This should be called
 * frequently after each object is copied, to achieve better locality and cache
 * usage.
 *
 * max_objs is the maximum number of objects to scan, or -1 to scan until the stack is
 * empty.
 */
gboolean
sgen_drain_gray_stack (int max_objs, ScanCopyContext ctx)
{
	ScanObjectFunc scan_func = ctx.ops->scan_object;
	GrayQueue *queue = ctx.queue;

	if (current_collection_generation == GENERATION_OLD && major_collector.drain_gray_stack)
		return major_collector.drain_gray_stack (ctx);

	do {
		int i;
		for (i = 0; i != max_objs; ++i) {
			char *obj;
			mword desc;
			GRAY_OBJECT_DEQUEUE (queue, &obj, &desc);
			if (!obj)
				return TRUE;
			SGEN_LOG (9, "Precise gray object scan %p (%s)", obj, sgen_client_object_safe_name ((GCObject*)obj));
			scan_func (obj, desc, queue);
		}
	} while (max_objs < 0);
	return FALSE;
}

/*
 * Addresses in the pin queue are already sorted. This function finds
 * the object header for each address and pins the object. The
 * addresses must be inside the nursery section.  The (start of the)
 * address array is overwritten with the addresses of the actually
 * pinned objects.  Return the number of pinned objects.
 */
static int
pin_objects_from_nursery_pin_queue (gboolean do_scan_objects, ScanCopyContext ctx)
{
	GCMemSection *section = nursery_section;
	void **start =  sgen_pinning_get_entry (section->pin_queue_first_entry);
	void **end = sgen_pinning_get_entry (section->pin_queue_last_entry);
	void *start_nursery = section->data;
	void *end_nursery = section->next_data;
	void *last = NULL;
	int count = 0;
	void *search_start;
	void *addr;
	void *pinning_front = start_nursery;
	size_t idx;
	void **definitely_pinned = start;
	ScanObjectFunc scan_func = ctx.ops->scan_object;
	SgenGrayQueue *queue = ctx.queue;

	sgen_nursery_allocator_prepare_for_pinning ();

	while (start < end) {
		void *obj_to_pin = NULL;
		size_t obj_to_pin_size = 0;
		mword desc;

		addr = *start;

		SGEN_ASSERT (0, addr >= start_nursery && addr < end_nursery, "Potential pinning address out of range");
		SGEN_ASSERT (0, addr >= last, "Pin queue not sorted");

		if (addr == last) {
			++start;
			continue;
		}

		SGEN_LOG (5, "Considering pinning addr %p", addr);
		/* We've already processed everything up to pinning_front. */
		if (addr < pinning_front) {
			start++;
			continue;
		}

		/*
		 * Find the closest scan start <= addr.  We might search backward in the
		 * scan_starts array because entries might be NULL.  In the worst case we
		 * start at start_nursery.
		 */
		idx = ((char*)addr - (char*)section->data) / SCAN_START_SIZE;
		SGEN_ASSERT (0, idx < section->num_scan_start, "Scan start index out of range");
		search_start = (void*)section->scan_starts [idx];
		if (!search_start || search_start > addr) {
			while (idx) {
				--idx;
				search_start = section->scan_starts [idx];
				if (search_start && search_start <= addr)
					break;
			}
			if (!search_start || search_start > addr)
				search_start = start_nursery;
		}

		/*
		 * If the pinning front is closer than the scan start we found, start
		 * searching at the front.
		 */
		if (search_start < pinning_front)
			search_start = pinning_front;

		/*
		 * Now addr should be in an object a short distance from search_start.
		 *
		 * search_start must point to zeroed mem or point to an object.
		 */
		do {
			size_t obj_size, canarified_obj_size;

			/* Skip zeros. */
			if (!*(void**)search_start) {
				search_start = (void*)ALIGN_UP ((mword)search_start + sizeof (gpointer));
				/* The loop condition makes sure we don't overrun addr. */
				continue;
			}

			canarified_obj_size = obj_size = ALIGN_UP (safe_object_get_size ((GCObject*)search_start));

			/*
			 * Filler arrays are marked by an invalid sync word.  We don't
			 * consider them for pinning.  They are not delimited by canaries,
			 * either.
			 */
			if (!sgen_client_object_is_array_fill ((GCObject*)search_start)) {
				CHECK_CANARY_FOR_OBJECT (search_start);
				CANARIFY_SIZE (canarified_obj_size);

				if (addr >= search_start && (char*)addr < (char*)search_start + obj_size) {
					/* This is the object we're looking for. */
					obj_to_pin = search_start;
					obj_to_pin_size = canarified_obj_size;
					break;
				}
			}

			/* Skip to the next object */
			search_start = (void*)((char*)search_start + canarified_obj_size);
		} while (search_start <= addr);

		/* We've searched past the address we were looking for. */
		if (!obj_to_pin) {
			pinning_front = search_start;
			goto next_pin_queue_entry;
		}

		/*
		 * We've found an object to pin.  It might still be a dummy array, but we
		 * can advance the pinning front in any case.
		 */
		pinning_front = (char*)obj_to_pin + obj_to_pin_size;

		/*
		 * If this is a dummy array marking the beginning of a nursery
		 * fragment, we don't pin it.
		 */
		if (sgen_client_object_is_array_fill ((GCObject*)obj_to_pin))
			goto next_pin_queue_entry;

		/*
		 * Finally - pin the object!
		 */
		desc = sgen_obj_get_descriptor_safe (obj_to_pin);
		if (do_scan_objects) {
			scan_func (obj_to_pin, desc, queue);
		} else {
			SGEN_LOG (4, "Pinned object %p, vtable %p (%s), count %d\n",
					obj_to_pin, *(void**)obj_to_pin, sgen_client_object_safe_name (obj_to_pin), count);
			binary_protocol_pin (obj_to_pin,
					(gpointer)LOAD_VTABLE (obj_to_pin),
					safe_object_get_size (obj_to_pin));

			pin_object (obj_to_pin);
			GRAY_OBJECT_ENQUEUE (queue, obj_to_pin, desc);
			sgen_pin_stats_register_object (obj_to_pin, obj_to_pin_size);
			definitely_pinned [count] = obj_to_pin;
			count++;
		}

	next_pin_queue_entry:
		last = addr;
		++start;
	}
	sgen_client_nursery_objects_pinned (definitely_pinned, count);
	stat_pinned_objects += count;
	return count;
}

static void
pin_objects_in_nursery (gboolean do_scan_objects, ScanCopyContext ctx)
{
	size_t reduced_to;

	if (nursery_section->pin_queue_first_entry == nursery_section->pin_queue_last_entry)
		return;

	reduced_to = pin_objects_from_nursery_pin_queue (do_scan_objects, ctx);
	nursery_section->pin_queue_last_entry = nursery_section->pin_queue_first_entry + reduced_to;
}

/*
 * This function is only ever called (via `collector_pin_object()` in `sgen-copy-object.h`)
 * when we can't promote an object because we're out of memory.
 */
void
sgen_pin_object (void *object, GrayQueue *queue)
{
	/*
	 * All pinned objects are assumed to have been staged, so we need to stage as well.
	 * Also, the count of staged objects shows that "late pinning" happened.
	 */
	sgen_pin_stage_ptr (object);

	SGEN_PIN_OBJECT (object);
	binary_protocol_pin (object, (gpointer)LOAD_VTABLE (object), safe_object_get_size (object));

	++objects_pinned;
	sgen_pin_stats_register_object (object, safe_object_get_size (object));

	GRAY_OBJECT_ENQUEUE (queue, object, sgen_obj_get_descriptor_safe (object));
}

/* Sort the addresses in array in increasing order.
 * Done using a by-the book heap sort. Which has decent and stable performance, is pretty cache efficient.
 */
void
sgen_sort_addresses (void **array, size_t size)
{
	size_t i;
	void *tmp;

	for (i = 1; i < size; ++i) {
		size_t child = i;
		while (child > 0) {
			size_t parent = (child - 1) / 2;

			if (array [parent] >= array [child])
				break;

			tmp = array [parent];
			array [parent] = array [child];
			array [child] = tmp;

			child = parent;
		}
	}

	for (i = size - 1; i > 0; --i) {
		size_t end, root;
		tmp = array [i];
		array [i] = array [0];
		array [0] = tmp;

		end = i - 1;
		root = 0;

		while (root * 2 + 1 <= end) {
			size_t child = root * 2 + 1;

			if (child < end && array [child] < array [child + 1])
				++child;
			if (array [root] >= array [child])
				break;

			tmp = array [root];
			array [root] = array [child];
			array [child] = tmp;

			root = child;
		}
	}
}

/* 
 * Scan the memory between start and end and queue values which could be pointers
 * to the area between start_nursery and end_nursery for later consideration.
 * Typically used for thread stacks.
 */
static void
conservatively_pin_objects_from (void **start, void **end, void *start_nursery, void *end_nursery, int pin_type)
{
	int count = 0;

#ifdef VALGRIND_MAKE_MEM_DEFINED_IF_ADDRESSABLE
	VALGRIND_MAKE_MEM_DEFINED_IF_ADDRESSABLE (start, (char*)end - (char*)start);
#endif

	while (start < end) {
		if (*start >= start_nursery && *start < end_nursery) {
			/*
			 * *start can point to the middle of an object
			 * note: should we handle pointing at the end of an object?
			 * pinning in C# code disallows pointing at the end of an object
			 * but there is some small chance that an optimizing C compiler
			 * may keep the only reference to an object by pointing
			 * at the end of it. We ignore this small chance for now.
			 * Pointers to the end of an object are indistinguishable
			 * from pointers to the start of the next object in memory
			 * so if we allow that we'd need to pin two objects...
			 * We queue the pointer in an array, the
			 * array will then be sorted and uniqued. This way
			 * we can coalesce several pinning pointers and it should
			 * be faster since we'd do a memory scan with increasing
			 * addresses. Note: we can align the address to the allocation
			 * alignment, so the unique process is more effective.
			 */
			mword addr = (mword)*start;
			addr &= ~(ALLOC_ALIGN - 1);
			if (addr >= (mword)start_nursery && addr < (mword)end_nursery) {
				SGEN_LOG (6, "Pinning address %p from %p", (void*)addr, start);
				sgen_pin_stage_ptr ((void*)addr);
				binary_protocol_pin_stage (start, (void*)addr);
				count++;
			}

			/*
			 * FIXME: It seems we're registering objects from all over the heap
			 * (at least from the nursery and the LOS), but we're only
			 * registering pinned addresses in the nursery.  What's up with
			 * that?
			 *
			 * Also, why wouldn't we register addresses once the pinning queue
			 * is sorted and uniqued?
			 */
			if (ptr_in_nursery ((void*)addr))
				sgen_pin_stats_register_address ((char*)addr, pin_type);
		}
		start++;
	}
	if (count)
		SGEN_LOG (7, "found %d potential pinned heap pointers", count);
}

/*
 * The first thing we do in a collection is to identify pinned objects.
 * This function considers all the areas of memory that need to be
 * conservatively scanned.
 */
static void
pin_from_roots (void *start_nursery, void *end_nursery, ScanCopyContext ctx)
{
	void **start_root;
	RootRecord *root;
	SGEN_LOG (2, "Scanning pinned roots (%d bytes, %d/%d entries)", (int)roots_size, roots_hash [ROOT_TYPE_NORMAL].num_entries, roots_hash [ROOT_TYPE_PINNED].num_entries);
	/* objects pinned from the API are inside these roots */
	SGEN_HASH_TABLE_FOREACH (&roots_hash [ROOT_TYPE_PINNED], start_root, root) {
		SGEN_LOG (6, "Pinned roots %p-%p", start_root, root->end_root);
		conservatively_pin_objects_from (start_root, (void**)root->end_root, start_nursery, end_nursery, PIN_TYPE_OTHER);
	} SGEN_HASH_TABLE_FOREACH_END;
	/* now deal with the thread stacks
	 * in the future we should be able to conservatively scan only:
	 * *) the cpu registers
	 * *) the unmanaged stack frames
	 * *) the _last_ managed stack frame
	 * *) pointers slots in managed frames
	 */
	scan_thread_data (start_nursery, end_nursery, FALSE, ctx);
}

static void
unpin_objects_from_queue (SgenGrayQueue *queue)
{
	for (;;) {
		char *addr;
		mword desc;
		GRAY_OBJECT_DEQUEUE (queue, &addr, &desc);
		if (!addr)
			break;
		g_assert (SGEN_OBJECT_IS_PINNED (addr));
		SGEN_UNPIN_OBJECT (addr);
	}
}

static void
single_arg_user_copy_or_mark (void **obj, void *gc_data)
{
	ScanCopyContext *ctx = gc_data;
	ctx->ops->copy_or_mark_object (obj, ctx->queue);
}

/*
 * The memory area from start_root to end_root contains pointers to objects.
 * Their position is precisely described by @desc (this means that the pointer
 * can be either NULL or the pointer to the start of an object).
 * This functions copies them to to_space updates them.
 *
 * This function is not thread-safe!
 */
static void
precisely_scan_objects_from (void** start_root, void** end_root, char* n_start, char *n_end, mword desc, ScanCopyContext ctx)
{
	CopyOrMarkObjectFunc copy_func = ctx.ops->copy_or_mark_object;
	SgenGrayQueue *queue = ctx.queue;

	switch (desc & ROOT_DESC_TYPE_MASK) {
	case ROOT_DESC_BITMAP:
		desc >>= ROOT_DESC_TYPE_SHIFT;
		while (desc) {
			if ((desc & 1) && *start_root) {
				copy_func (start_root, queue);
				SGEN_LOG (9, "Overwrote root at %p with %p", start_root, *start_root);
			}
			desc >>= 1;
			start_root++;
		}
		return;
	case ROOT_DESC_COMPLEX: {
		gsize *bitmap_data = sgen_get_complex_descriptor_bitmap (desc);
		gsize bwords = (*bitmap_data) - 1;
		void **start_run = start_root;
		bitmap_data++;
		while (bwords-- > 0) {
			gsize bmap = *bitmap_data++;
			void **objptr = start_run;
			while (bmap) {
				if ((bmap & 1) && *objptr) {
					copy_func (objptr, queue);
					SGEN_LOG (9, "Overwrote root at %p with %p", objptr, *objptr);
				}
				bmap >>= 1;
				++objptr;
			}
			start_run += GC_BITS_PER_WORD;
		}
		break;
	}
	case ROOT_DESC_USER: {
		MonoGCRootMarkFunc marker = sgen_get_user_descriptor_func (desc);
		marker (start_root, single_arg_user_copy_or_mark, &ctx);
		break;
	}
	case ROOT_DESC_RUN_LEN:
		g_assert_not_reached ();
	default:
		g_assert_not_reached ();
	}
}

static void
reset_heap_boundaries (void)
{
	lowest_heap_address = ~(mword)0;
	highest_heap_address = 0;
}

void
sgen_update_heap_boundaries (mword low, mword high)
{
	mword old;

	do {
		old = lowest_heap_address;
		if (low >= old)
			break;
	} while (SGEN_CAS_PTR ((gpointer*)&lowest_heap_address, (gpointer)low, (gpointer)old) != (gpointer)old);

	do {
		old = highest_heap_address;
		if (high <= old)
			break;
	} while (SGEN_CAS_PTR ((gpointer*)&highest_heap_address, (gpointer)high, (gpointer)old) != (gpointer)old);
}

/*
 * Allocate and setup the data structures needed to be able to allocate objects
 * in the nursery. The nursery is stored in nursery_section.
 */
static void
alloc_nursery (void)
{
	GCMemSection *section;
	char *data;
	size_t scan_starts;
	size_t alloc_size;

	if (nursery_section)
		return;
	SGEN_LOG (2, "Allocating nursery size: %zu", (size_t)sgen_nursery_size);
	/* later we will alloc a larger area for the nursery but only activate
	 * what we need. The rest will be used as expansion if we have too many pinned
	 * objects in the existing nursery.
	 */
	/* FIXME: handle OOM */
	section = sgen_alloc_internal (INTERNAL_MEM_SECTION);

	alloc_size = sgen_nursery_size;

	/* If there isn't enough space even for the nursery we should simply abort. */
	g_assert (sgen_memgov_try_alloc_space (alloc_size, SPACE_NURSERY));

	data = major_collector.alloc_heap (alloc_size, alloc_size, DEFAULT_NURSERY_BITS);
	sgen_update_heap_boundaries ((mword)data, (mword)(data + sgen_nursery_size));
	SGEN_LOG (4, "Expanding nursery size (%p-%p): %lu, total: %lu", data, data + alloc_size, (unsigned long)sgen_nursery_size, (unsigned long)mono_gc_get_heap_size ());
	section->data = section->next_data = data;
	section->size = alloc_size;
	section->end_data = data + sgen_nursery_size;
	scan_starts = (alloc_size + SCAN_START_SIZE - 1) / SCAN_START_SIZE;
	section->scan_starts = sgen_alloc_internal_dynamic (sizeof (char*) * scan_starts, INTERNAL_MEM_SCAN_STARTS, TRUE);
	section->num_scan_start = scan_starts;

	nursery_section = section;

	sgen_nursery_allocator_set_nursery_bounds (data, data + sgen_nursery_size);
}

void*
mono_gc_get_nursery (int *shift_bits, size_t *size)
{
	*size = sgen_nursery_size;
	*shift_bits = DEFAULT_NURSERY_BITS;
	return sgen_get_nursery_start ();
}

gboolean
mono_gc_precise_stack_mark_enabled (void)
{
	return !conservative_stack_mark;
}

FILE *
mono_gc_get_logfile (void)
{
	return gc_debug_file;
}

static void
scan_finalizer_entries (SgenPointerQueue *fin_queue, ScanCopyContext ctx)
{
	CopyOrMarkObjectFunc copy_func = ctx.ops->copy_or_mark_object;
	SgenGrayQueue *queue = ctx.queue;
	size_t i;

	for (i = 0; i < fin_queue->next_slot; ++i) {
		void *obj = fin_queue->data [i];
		if (!obj)
			continue;
		SGEN_LOG (5, "Scan of fin ready object: %p (%s)\n", obj, sgen_client_object_safe_name (obj));
		copy_func (&fin_queue->data [i], queue);
	}
}

static const char*
generation_name (int generation)
{
	switch (generation) {
	case GENERATION_NURSERY: return "nursery";
	case GENERATION_OLD: return "old";
	default: g_assert_not_reached ();
	}
}

const char*
sgen_generation_name (int generation)
{
	return generation_name (generation);
}

static void
finish_gray_stack (int generation, ScanCopyContext ctx)
{
	TV_DECLARE (atv);
	TV_DECLARE (btv);
	int done_with_ephemerons, ephemeron_rounds = 0;
	char *start_addr = generation == GENERATION_NURSERY ? sgen_get_nursery_start () : NULL;
	char *end_addr = generation == GENERATION_NURSERY ? sgen_get_nursery_end () : (char*)-1;
	SgenGrayQueue *queue = ctx.queue;

	/*
	 * We copied all the reachable objects. Now it's the time to copy
	 * the objects that were not referenced by the roots, but by the copied objects.
	 * we built a stack of objects pointed to by gray_start: they are
	 * additional roots and we may add more items as we go.
	 * We loop until gray_start == gray_objects which means no more objects have
	 * been added. Note this is iterative: no recursion is involved.
	 * We need to walk the LO list as well in search of marked big objects
	 * (use a flag since this is needed only on major collections). We need to loop
	 * here as well, so keep a counter of marked LO (increasing it in copy_object).
	 *   To achieve better cache locality and cache usage, we drain the gray stack 
	 * frequently, after each object is copied, and just finish the work here.
	 */
	sgen_drain_gray_stack (-1, ctx);
	TV_GETTIME (atv);
	SGEN_LOG (2, "%s generation done", generation_name (generation));

	/*
	Reset bridge data, we might have lingering data from a previous collection if this is a major
	collection trigged by minor overflow.

	We must reset the gathered bridges since their original block might be evacuated due to major
	fragmentation in the meanwhile and the bridge code should not have to deal with that.
	*/
	if (sgen_need_bridge_processing ())
		sgen_bridge_reset_data ();

	/*
	 * Walk the ephemeron tables marking all values with reachable keys. This must be completely done
	 * before processing finalizable objects and non-tracking weak links to avoid finalizing/clearing
	 * objects that are in fact reachable.
	 */
	done_with_ephemerons = 0;
	do {
		done_with_ephemerons = sgen_client_mark_ephemerons (ctx);
		sgen_drain_gray_stack (-1, ctx);
		++ephemeron_rounds;
	} while (!done_with_ephemerons);

	sgen_mark_togglerefs (start_addr, end_addr, ctx);

	if (sgen_need_bridge_processing ()) {
		/*Make sure the gray stack is empty before we process bridge objects so we get liveness right*/
		sgen_drain_gray_stack (-1, ctx);
		sgen_collect_bridge_objects (generation, ctx);
		if (generation == GENERATION_OLD)
			sgen_collect_bridge_objects (GENERATION_NURSERY, ctx);

		/*
		Do the first bridge step here, as the collector liveness state will become useless after that.

		An important optimization is to only proccess the possibly dead part of the object graph and skip
		over all live objects as we transitively know everything they point must be alive too.

		The above invariant is completely wrong if we let the gray queue be drained and mark/copy everything.

		This has the unfortunate side effect of making overflow collections perform the first step twice, but
		given we now have heuristics that perform major GC in anticipation of minor overflows this should not
		be a big deal.
		*/
		sgen_bridge_processing_stw_step ();
	}

	/*
	Make sure we drain the gray stack before processing disappearing links and finalizers.
	If we don't make sure it is empty we might wrongly see a live object as dead.
	*/
	sgen_drain_gray_stack (-1, ctx);

	/*
	We must clear weak links that don't track resurrection before processing object ready for
	finalization so they can be cleared before that.
	*/
	sgen_null_link_in_range (generation, TRUE, ctx);
	if (generation == GENERATION_OLD)
		sgen_null_link_in_range (GENERATION_NURSERY, TRUE, ctx);


	/* walk the finalization queue and move also the objects that need to be
	 * finalized: use the finalized objects as new roots so the objects they depend
	 * on are also not reclaimed. As with the roots above, only objects in the nursery
	 * are marked/copied.
	 */
	sgen_finalize_in_range (generation, ctx);
	if (generation == GENERATION_OLD)
		sgen_finalize_in_range (GENERATION_NURSERY, ctx);
	/* drain the new stack that might have been created */
	SGEN_LOG (6, "Precise scan of gray area post fin");
	sgen_drain_gray_stack (-1, ctx);

	/*
	 * This must be done again after processing finalizable objects since CWL slots are cleared only after the key is finalized.
	 */
	done_with_ephemerons = 0;
	do {
		done_with_ephemerons = sgen_client_mark_ephemerons (ctx);
		sgen_drain_gray_stack (-1, ctx);
		++ephemeron_rounds;
	} while (!done_with_ephemerons);

	sgen_client_clear_unreachable_ephemerons (ctx);

	/*
	 * We clear togglerefs only after all possible chances of revival are done. 
	 * This is semantically more inline with what users expect and it allows for
	 * user finalizers to correctly interact with TR objects.
	*/
	sgen_clear_togglerefs (start_addr, end_addr, ctx);

	TV_GETTIME (btv);
	SGEN_LOG (2, "Finalize queue handling scan for %s generation: %d usecs %d ephemeron rounds", generation_name (generation), TV_ELAPSED (atv, btv), ephemeron_rounds);

	/*
	 * handle disappearing links
	 * Note we do this after checking the finalization queue because if an object
	 * survives (at least long enough to be finalized) we don't clear the link.
	 * This also deals with a possible issue with the monitor reclamation: with the Boehm
	 * GC a finalized object my lose the monitor because it is cleared before the finalizer is
	 * called.
	 */
	g_assert (sgen_gray_object_queue_is_empty (queue));
	for (;;) {
		sgen_null_link_in_range (generation, FALSE, ctx);
		if (generation == GENERATION_OLD)
			sgen_null_link_in_range (GENERATION_NURSERY, FALSE, ctx);
		if (sgen_gray_object_queue_is_empty (queue))
			break;
		sgen_drain_gray_stack (-1, ctx);
	}

	g_assert (sgen_gray_object_queue_is_empty (queue));

	sgen_gray_object_queue_trim_free_list (queue);
}

void
sgen_check_section_scan_starts (GCMemSection *section)
{
	size_t i;
	for (i = 0; i < section->num_scan_start; ++i) {
		if (section->scan_starts [i]) {
			mword size = safe_object_get_size ((GCObject*) section->scan_starts [i]);
			SGEN_ASSERT (0, size >= SGEN_CLIENT_MINIMUM_OBJECT_SIZE && size <= MAX_SMALL_OBJ_SIZE, "Weird object size at scan starts.");
		}
	}
}

static void
check_scan_starts (void)
{
	if (!do_scan_starts_check)
		return;
	sgen_check_section_scan_starts (nursery_section);
	major_collector.check_scan_starts ();
}

static void
scan_from_registered_roots (char *addr_start, char *addr_end, int root_type, ScanCopyContext ctx)
{
	void **start_root;
	RootRecord *root;
	SGEN_HASH_TABLE_FOREACH (&roots_hash [root_type], start_root, root) {
		SGEN_LOG (6, "Precise root scan %p-%p (desc: %p)", start_root, root->end_root, (void*)root->root_desc);
		precisely_scan_objects_from (start_root, (void**)root->end_root, addr_start, addr_end, root->root_desc, ctx);
	} SGEN_HASH_TABLE_FOREACH_END;
}

static void
init_stats (void)
{
	static gboolean inited = FALSE;

	if (inited)
		return;

	mono_counters_register ("Collection max time",  MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME | MONO_COUNTER_MONOTONIC, &time_max);

	mono_counters_register ("Minor fragment clear", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_minor_pre_collection_fragment_clear);
	mono_counters_register ("Minor pinning", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_minor_pinning);
	mono_counters_register ("Minor scan remembered set", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_minor_scan_remsets);
	mono_counters_register ("Minor scan pinned", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_minor_scan_pinned);
	mono_counters_register ("Minor scan roots", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_minor_scan_roots);
	mono_counters_register ("Minor fragment creation", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_minor_fragment_creation);

	mono_counters_register ("Major fragment clear", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_pre_collection_fragment_clear);
	mono_counters_register ("Major pinning", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_pinning);
	mono_counters_register ("Major scan pinned", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_scan_pinned);
	mono_counters_register ("Major scan roots", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_scan_roots);
	mono_counters_register ("Major scan mod union", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_scan_mod_union);
	mono_counters_register ("Major finish gray stack", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_finish_gray_stack);
	mono_counters_register ("Major free big objects", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_free_bigobjs);
	mono_counters_register ("Major LOS sweep", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_los_sweep);
	mono_counters_register ("Major sweep", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_sweep);
	mono_counters_register ("Major fragment creation", MONO_COUNTER_GC | MONO_COUNTER_ULONG | MONO_COUNTER_TIME, &time_major_fragment_creation);

	mono_counters_register ("Number of pinned objects", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_pinned_objects);

#ifdef HEAVY_STATISTICS
	mono_counters_register ("WBarrier remember pointer", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_add_to_global_remset);
	mono_counters_register ("WBarrier set field", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_field);
	mono_counters_register ("WBarrier set arrayref", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_arrayref);
	mono_counters_register ("WBarrier arrayref copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_arrayref_copy);
	mono_counters_register ("WBarrier generic store called", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_generic_store);
	mono_counters_register ("WBarrier generic atomic store called", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_generic_store_atomic);
	mono_counters_register ("WBarrier set root", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_root);
	mono_counters_register ("WBarrier value copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_value_copy);
	mono_counters_register ("WBarrier object copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_object_copy);

	mono_counters_register ("# objects allocated degraded", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_objects_alloced_degraded);
	mono_counters_register ("bytes allocated degraded", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_bytes_alloced_degraded);

	mono_counters_register ("# copy_object() called (nursery)", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_copy_object_called_nursery);
	mono_counters_register ("# objects copied (nursery)", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_objects_copied_nursery);
	mono_counters_register ("# copy_object() called (major)", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_copy_object_called_major);
	mono_counters_register ("# objects copied (major)", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_objects_copied_major);

	mono_counters_register ("# scan_object() called (nursery)", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_scan_object_called_nursery);
	mono_counters_register ("# scan_object() called (major)", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_scan_object_called_major);

	mono_counters_register ("Slots allocated in vain", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_slots_allocated_in_vain);

	mono_counters_register ("# nursery copy_object() failed from space", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_nursery_copy_object_failed_from_space);
	mono_counters_register ("# nursery copy_object() failed forwarded", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_nursery_copy_object_failed_forwarded);
	mono_counters_register ("# nursery copy_object() failed pinned", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_nursery_copy_object_failed_pinned);
	mono_counters_register ("# nursery copy_object() failed to space", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_nursery_copy_object_failed_to_space);

	sgen_nursery_allocator_init_heavy_stats ();
#endif

	inited = TRUE;
}


static void
reset_pinned_from_failed_allocation (void)
{
	bytes_pinned_from_failed_allocation = 0;
}

void
sgen_set_pinned_from_failed_allocation (mword objsize)
{
	bytes_pinned_from_failed_allocation += objsize;
}

gboolean
sgen_collection_is_concurrent (void)
{
	switch (current_collection_generation) {
	case GENERATION_NURSERY:
		return FALSE;
	case GENERATION_OLD:
		return concurrent_collection_in_progress;
	default:
		g_error ("Invalid current generation %d", current_collection_generation);
	}
}

gboolean
sgen_concurrent_collection_in_progress (void)
{
	return concurrent_collection_in_progress;
}

typedef struct {
	SgenThreadPoolJob job;
	SgenObjectOperations *ops;
} ScanJob;

static void
job_remembered_set_scan (void *worker_data_untyped, SgenThreadPoolJob *job)
{
	WorkerData *worker_data = worker_data_untyped;
	ScanJob *job_data = (ScanJob*)job;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (job_data->ops, sgen_workers_get_job_gray_queue (worker_data));
	remset.scan_remsets (ctx);
}

typedef struct {
	SgenThreadPoolJob job;
	SgenObjectOperations *ops;
	char *heap_start;
	char *heap_end;
	int root_type;
} ScanFromRegisteredRootsJob;

static void
job_scan_from_registered_roots (void *worker_data_untyped, SgenThreadPoolJob *job)
{
	WorkerData *worker_data = worker_data_untyped;
	ScanFromRegisteredRootsJob *job_data = (ScanFromRegisteredRootsJob*)job;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (job_data->ops, sgen_workers_get_job_gray_queue (worker_data));

	scan_from_registered_roots (job_data->heap_start, job_data->heap_end, job_data->root_type, ctx);
}

typedef struct {
	SgenThreadPoolJob job;
	SgenObjectOperations *ops;
	char *heap_start;
	char *heap_end;
} ScanThreadDataJob;

static void
job_scan_thread_data (void *worker_data_untyped, SgenThreadPoolJob *job)
{
	WorkerData *worker_data = worker_data_untyped;
	ScanThreadDataJob *job_data = (ScanThreadDataJob*)job;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (job_data->ops, sgen_workers_get_job_gray_queue (worker_data));

	scan_thread_data (job_data->heap_start, job_data->heap_end, TRUE, ctx);
}

typedef struct {
	SgenThreadPoolJob job;
	SgenObjectOperations *ops;
	SgenPointerQueue *queue;
} ScanFinalizerEntriesJob;

static void
job_scan_finalizer_entries (void *worker_data_untyped, SgenThreadPoolJob *job)
{
	WorkerData *worker_data = worker_data_untyped;
	ScanFinalizerEntriesJob *job_data = (ScanFinalizerEntriesJob*)job;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (job_data->ops, sgen_workers_get_job_gray_queue (worker_data));

	scan_finalizer_entries (job_data->queue, ctx);
}

static void
job_scan_major_mod_union_card_table (void *worker_data_untyped, SgenThreadPoolJob *job)
{
	WorkerData *worker_data = worker_data_untyped;
	ScanJob *job_data = (ScanJob*)job;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (job_data->ops, sgen_workers_get_job_gray_queue (worker_data));

	g_assert (concurrent_collection_in_progress);
	major_collector.scan_card_table (TRUE, ctx);
}

static void
job_scan_los_mod_union_card_table (void *worker_data_untyped, SgenThreadPoolJob *job)
{
	WorkerData *worker_data = worker_data_untyped;
	ScanJob *job_data = (ScanJob*)job;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (job_data->ops, sgen_workers_get_job_gray_queue (worker_data));

	g_assert (concurrent_collection_in_progress);
	sgen_los_scan_card_table (TRUE, ctx);
}

static void
init_gray_queue (void)
{
	if (sgen_collection_is_concurrent ())
		sgen_workers_init_distribute_gray_queue ();
	sgen_gray_object_queue_init (&gray_queue, NULL);
}

static void
enqueue_scan_from_roots_jobs (char *heap_start, char *heap_end, SgenObjectOperations *ops)
{
	ScanFromRegisteredRootsJob *scrrj;
	ScanThreadDataJob *stdj;
	ScanFinalizerEntriesJob *sfej;

	/* registered roots, this includes static fields */

	scrrj = (ScanFromRegisteredRootsJob*)sgen_thread_pool_job_alloc ("scan from registered roots normal", job_scan_from_registered_roots, sizeof (ScanFromRegisteredRootsJob));
	scrrj->ops = ops;
	scrrj->heap_start = heap_start;
	scrrj->heap_end = heap_end;
	scrrj->root_type = ROOT_TYPE_NORMAL;
	sgen_workers_enqueue_job (&scrrj->job);

	scrrj = (ScanFromRegisteredRootsJob*)sgen_thread_pool_job_alloc ("scan from registered roots wbarrier", job_scan_from_registered_roots, sizeof (ScanFromRegisteredRootsJob));
	scrrj->ops = ops;
	scrrj->heap_start = heap_start;
	scrrj->heap_end = heap_end;
	scrrj->root_type = ROOT_TYPE_WBARRIER;
	sgen_workers_enqueue_job (&scrrj->job);

	/* Threads */

	stdj = (ScanThreadDataJob*)sgen_thread_pool_job_alloc ("scan thread data", job_scan_thread_data, sizeof (ScanThreadDataJob));
	stdj->heap_start = heap_start;
	stdj->heap_end = heap_end;
	sgen_workers_enqueue_job (&stdj->job);

	/* Scan the list of objects ready for finalization. */

	sfej = (ScanFinalizerEntriesJob*)sgen_thread_pool_job_alloc ("scan finalizer entries", job_scan_finalizer_entries, sizeof (ScanFinalizerEntriesJob));
	sfej->queue = &fin_ready_queue;
	sfej->ops = ops;
	sgen_workers_enqueue_job (&sfej->job);

	sfej = (ScanFinalizerEntriesJob*)sgen_thread_pool_job_alloc ("scan critical finalizer entries", job_scan_finalizer_entries, sizeof (ScanFinalizerEntriesJob));
	sfej->queue = &critical_fin_queue;
	sfej->ops = ops;
	sgen_workers_enqueue_job (&sfej->job);
}

/*
 * Perform a nursery collection.
 *
 * Return whether any objects were late-pinned due to being out of memory.
 */
static gboolean
collect_nursery (SgenGrayQueue *unpin_queue, gboolean finish_up_concurrent_mark)
{
	gboolean needs_major;
	size_t max_garbage_amount;
	char *nursery_next;
	mword fragment_total;
	ScanJob *sj;
	SgenObjectOperations *object_ops = &sgen_minor_collector.serial_ops;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (object_ops, &gray_queue);
	TV_DECLARE (atv);
	TV_DECLARE (btv);

	if (disable_minor_collections)
		return TRUE;

	TV_GETTIME (last_minor_collection_start_tv);
	atv = last_minor_collection_start_tv;

	binary_protocol_collection_begin (gc_stats.minor_gc_count, GENERATION_NURSERY);

	if (do_verify_nursery || do_dump_nursery_content)
		sgen_debug_verify_nursery (do_dump_nursery_content);

#ifndef DISABLE_PERFCOUNTERS
	mono_perfcounters->gc_collections0++;
#endif

	current_collection_generation = GENERATION_NURSERY;

	SGEN_ASSERT (0, !sgen_collection_is_concurrent (), "Why is the nursery collection concurrent?");

	reset_pinned_from_failed_allocation ();

	check_scan_starts ();

	sgen_nursery_alloc_prepare_for_minor ();

	degraded_mode = 0;
	objects_pinned = 0;
	nursery_next = sgen_nursery_alloc_get_upper_alloc_bound ();
	/* FIXME: optimize later to use the higher address where an object can be present */
	nursery_next = MAX (nursery_next, sgen_get_nursery_end ());

	SGEN_LOG (1, "Start nursery collection %d %p-%p, size: %d", gc_stats.minor_gc_count, sgen_get_nursery_start (), nursery_next, (int)(nursery_next - sgen_get_nursery_start ()));
	max_garbage_amount = nursery_next - sgen_get_nursery_start ();
	g_assert (nursery_section->size >= max_garbage_amount);

	/* world must be stopped already */
	TV_GETTIME (btv);
	time_minor_pre_collection_fragment_clear += TV_ELAPSED (atv, btv);

	sgen_client_pre_collection_checks ();

	nursery_section->next_data = nursery_next;

	major_collector.start_nursery_collection ();

	sgen_memgov_minor_collection_start ();

	init_gray_queue ();

	gc_stats.minor_gc_count ++;

	if (whole_heap_check_before_collection) {
		sgen_clear_nursery_fragments ();
		sgen_check_whole_heap (finish_up_concurrent_mark);
	}
	if (consistency_check_at_minor_collection)
		sgen_check_consistency ();

	sgen_process_fin_stage_entries ();
	sgen_process_dislink_stage_entries ();

	/* pin from pinned handles */
	sgen_init_pinning ();
	sgen_client_binary_protocol_mark_start (GENERATION_NURSERY);
	pin_from_roots (sgen_get_nursery_start (), nursery_next, ctx);
	/* pin cemented objects */
	sgen_pin_cemented_objects ();
	/* identify pinned objects */
	sgen_optimize_pin_queue ();
	sgen_pinning_setup_section (nursery_section);

	pin_objects_in_nursery (FALSE, ctx);
	sgen_pinning_trim_queue_to_section (nursery_section);

	TV_GETTIME (atv);
	time_minor_pinning += TV_ELAPSED (btv, atv);
	SGEN_LOG (2, "Finding pinned pointers: %zd in %d usecs", sgen_get_pinned_count (), TV_ELAPSED (btv, atv));
	SGEN_LOG (4, "Start scan with %zd pinned objects", sgen_get_pinned_count ());

	/*
	 * FIXME: When we finish a concurrent collection we do a nursery collection first,
	 * as part of which we scan the card table.  Then, later, we scan the mod union
	 * cardtable.  We should only have to do one.
	 */
	sj = (ScanJob*)sgen_thread_pool_job_alloc ("scan remset", job_remembered_set_scan, sizeof (ScanJob));
	sj->ops = object_ops;
	sgen_workers_enqueue_job (&sj->job);

	/* we don't have complete write barrier yet, so we scan all the old generation sections */
	TV_GETTIME (btv);
	time_minor_scan_remsets += TV_ELAPSED (atv, btv);
	SGEN_LOG (2, "Old generation scan: %d usecs", TV_ELAPSED (atv, btv));

	sgen_drain_gray_stack (-1, ctx);

	/* FIXME: Why do we do this at this specific, seemingly random, point? */
	sgen_client_collecting_minor (&fin_ready_queue, &critical_fin_queue);

	TV_GETTIME (atv);
	time_minor_scan_pinned += TV_ELAPSED (btv, atv);

	enqueue_scan_from_roots_jobs (sgen_get_nursery_start (), nursery_next, object_ops);

	TV_GETTIME (btv);
	time_minor_scan_roots += TV_ELAPSED (atv, btv);

	finish_gray_stack (GENERATION_NURSERY, ctx);

	TV_GETTIME (atv);
	time_minor_finish_gray_stack += TV_ELAPSED (btv, atv);
	sgen_client_binary_protocol_mark_end (GENERATION_NURSERY);

	if (objects_pinned) {
		sgen_optimize_pin_queue ();
		sgen_pinning_setup_section (nursery_section);
	}

	/* walk the pin_queue, build up the fragment list of free memory, unmark
	 * pinned objects as we go, memzero() the empty fragments so they are ready for the
	 * next allocations.
	 */
	sgen_client_binary_protocol_reclaim_start (GENERATION_NURSERY);
	fragment_total = sgen_build_nursery_fragments (nursery_section, unpin_queue);
	if (!fragment_total)
		degraded_mode = 1;

	/* Clear TLABs for all threads */
	sgen_clear_tlabs ();

	sgen_client_binary_protocol_reclaim_end (GENERATION_NURSERY);
	TV_GETTIME (btv);
	time_minor_fragment_creation += TV_ELAPSED (atv, btv);
	SGEN_LOG (2, "Fragment creation: %d usecs, %lu bytes available", TV_ELAPSED (atv, btv), (unsigned long)fragment_total);

	if (consistency_check_at_minor_collection)
		sgen_check_major_refs ();

	major_collector.finish_nursery_collection ();

	TV_GETTIME (last_minor_collection_end_tv);
	gc_stats.minor_gc_time += TV_ELAPSED (last_minor_collection_start_tv, last_minor_collection_end_tv);

	sgen_debug_dump_heap ("minor", gc_stats.minor_gc_count - 1, NULL);

	/* prepare the pin queue for the next collection */
	sgen_finish_pinning ();
	if (mono_gc_pending_finalizers ()) {
		SGEN_LOG (4, "Finalizer-thread wakeup");
		mono_gc_finalize_notify ();
	}
	sgen_pin_stats_reset ();
	/* clear cemented hash */
	sgen_cement_clear_below_threshold ();

	g_assert (sgen_gray_object_queue_is_empty (&gray_queue));

	remset.finish_minor_collection ();

	check_scan_starts ();

	binary_protocol_flush_buffers (FALSE);

	sgen_memgov_minor_collection_end ();

	/*objects are late pinned because of lack of memory, so a major is a good call*/
	needs_major = objects_pinned > 0;
	current_collection_generation = -1;
	objects_pinned = 0;

	binary_protocol_collection_end (gc_stats.minor_gc_count - 1, GENERATION_NURSERY, 0, 0);

	if (check_nursery_objects_pinned && !sgen_minor_collector.is_split)
		sgen_check_nursery_objects_pinned (unpin_queue != NULL);

	return needs_major;
}

static void
scan_nursery_objects_callback (char *obj, size_t size, ScanCopyContext *ctx)
{
	/*
	 * This is called on all objects in the nursery, including pinned ones, so we need
	 * to use sgen_obj_get_descriptor_safe(), which masks out the vtable tag bits.
	 */
	ctx->ops->scan_object (obj, sgen_obj_get_descriptor_safe (obj), ctx->queue);
}

static void
scan_nursery_objects (ScanCopyContext ctx)
{
	sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
			(IterateObjectCallbackFunc)scan_nursery_objects_callback, (void*)&ctx, FALSE);
}

typedef enum {
	COPY_OR_MARK_FROM_ROOTS_SERIAL,
	COPY_OR_MARK_FROM_ROOTS_START_CONCURRENT,
	COPY_OR_MARK_FROM_ROOTS_FINISH_CONCURRENT
} CopyOrMarkFromRootsMode;

static void
major_copy_or_mark_from_roots (size_t *old_next_pin_slot, CopyOrMarkFromRootsMode mode, gboolean scan_whole_nursery, SgenObjectOperations *object_ops)
{
	LOSObject *bigobj;
	TV_DECLARE (atv);
	TV_DECLARE (btv);
	/* FIXME: only use these values for the precise scan
	 * note that to_space pointers should be excluded anyway...
	 */
	char *heap_start = NULL;
	char *heap_end = (char*)-1;
	ScanCopyContext ctx = CONTEXT_FROM_OBJECT_OPERATIONS (object_ops, WORKERS_DISTRIBUTE_GRAY_QUEUE);
	gboolean concurrent = mode != COPY_OR_MARK_FROM_ROOTS_SERIAL;

	SGEN_ASSERT (0, !!concurrent == !!concurrent_collection_in_progress, "We've been called with the wrong mode.");

	if (scan_whole_nursery)
		SGEN_ASSERT (0, mode == COPY_OR_MARK_FROM_ROOTS_FINISH_CONCURRENT, "Scanning whole nursery only makes sense when we're finishing a concurrent collection.");

	if (concurrent) {
		/*This cleans up unused fragments */
		sgen_nursery_allocator_prepare_for_pinning ();

		if (do_concurrent_checks)
			sgen_debug_check_nursery_is_clean ();
	} else {
		/* The concurrent collector doesn't touch the nursery. */
		sgen_nursery_alloc_prepare_for_major ();
	}

	init_gray_queue ();

	TV_GETTIME (atv);

	/* Pinning depends on this */
	sgen_clear_nursery_fragments ();

	if (whole_heap_check_before_collection)
		sgen_check_whole_heap (mode == COPY_OR_MARK_FROM_ROOTS_FINISH_CONCURRENT);

	TV_GETTIME (btv);
	time_major_pre_collection_fragment_clear += TV_ELAPSED (atv, btv);

	if (!sgen_collection_is_concurrent ())
		nursery_section->next_data = sgen_get_nursery_end ();
	/* we should also coalesce scanning from sections close to each other
	 * and deal with pointers outside of the sections later.
	 */

	objects_pinned = 0;

	sgen_client_pre_collection_checks ();

	if (!concurrent) {
		/* Remsets are not useful for a major collection */
		remset.clear_cards ();
	}

	sgen_process_fin_stage_entries ();
	sgen_process_dislink_stage_entries ();

	TV_GETTIME (atv);
	sgen_init_pinning ();
	SGEN_LOG (6, "Collecting pinned addresses");
	pin_from_roots ((void*)lowest_heap_address, (void*)highest_heap_address, ctx);

	if (mode != COPY_OR_MARK_FROM_ROOTS_START_CONCURRENT) {
		if (major_collector.is_concurrent) {
			/*
			 * The concurrent major collector cannot evict
			 * yet, so we need to pin cemented objects to
			 * not break some asserts.
			 *
			 * FIXME: We could evict now!
			 */
			sgen_pin_cemented_objects ();
		}
	}

	sgen_optimize_pin_queue ();

	sgen_client_collecting_major_1 ();

	/*
	 * pin_queue now contains all candidate pointers, sorted and
	 * uniqued.  We must do two passes now to figure out which
	 * objects are pinned.
	 *
	 * The first is to find within the pin_queue the area for each
	 * section.  This requires that the pin_queue be sorted.  We
	 * also process the LOS objects and pinned chunks here.
	 *
	 * The second, destructive, pass is to reduce the section
	 * areas to pointers to the actually pinned objects.
	 */
	SGEN_LOG (6, "Pinning from sections");
	/* first pass for the sections */
	sgen_find_section_pin_queue_start_end (nursery_section);
	/* identify possible pointers to the insize of large objects */
	SGEN_LOG (6, "Pinning from large objects");
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		size_t dummy;
		if (sgen_find_optimized_pin_queue_area (bigobj->data, (char*)bigobj->data + sgen_los_object_size (bigobj), &dummy, &dummy)) {
			binary_protocol_pin (bigobj->data, (gpointer)LOAD_VTABLE (bigobj->data), safe_object_get_size (((GCObject*)(bigobj->data))));

			if (sgen_los_object_is_pinned (bigobj->data)) {
				SGEN_ASSERT (0, mode == COPY_OR_MARK_FROM_ROOTS_FINISH_CONCURRENT, "LOS objects can only be pinned here after concurrent marking.");
				continue;
			}
			sgen_los_pin_object (bigobj->data);
			if (SGEN_OBJECT_HAS_REFERENCES (bigobj->data))
				GRAY_OBJECT_ENQUEUE (WORKERS_DISTRIBUTE_GRAY_QUEUE, bigobj->data, sgen_obj_get_descriptor (bigobj->data));
			sgen_pin_stats_register_object ((char*) bigobj->data, safe_object_get_size ((GCObject*) bigobj->data));
			SGEN_LOG (6, "Marked large object %p (%s) size: %lu from roots", bigobj->data,
					sgen_client_object_safe_name ((GCObject*)bigobj->data),
					(unsigned long)sgen_los_object_size (bigobj));

			sgen_client_pinned_los_object (bigobj->data);
		}
	}
	/* second pass for the sections */

	/*
	 * Concurrent mark never follows references into the nursery.  In the start and
	 * finish pauses we must scan live nursery objects, though.
	 *
	 * In the finish pause we do this conservatively by scanning all nursery objects.
	 * Previously we would only scan pinned objects here.  We assumed that all objects
	 * that were pinned during the nursery collection immediately preceding this finish
	 * mark would be pinned again here.  Due to the way we get the stack end for the GC
	 * thread, however, that's not necessarily the case: we scan part of the stack used
	 * by the GC itself, which changes constantly, so pinning isn't entirely
	 * deterministic.
	 *
	 * The split nursery also complicates things because non-pinned objects can survive
	 * in the nursery.  That's why we need to do a full scan of the nursery for it, too.
	 *
	 * In the future we shouldn't do a preceding nursery collection at all and instead
	 * do the finish pause with promotion from the nursery.
	 *
	 * A further complication arises when we have late-pinned objects from the preceding
	 * nursery collection.  Those are the result of being out of memory when trying to
	 * evacuate objects.  They won't be found from the roots, so we just scan the whole
	 * nursery.
	 *
	 * Non-concurrent mark evacuates from the nursery, so it's
	 * sufficient to just scan pinned nursery objects.
	 */
	if (scan_whole_nursery || mode == COPY_OR_MARK_FROM_ROOTS_FINISH_CONCURRENT || (concurrent && sgen_minor_collector.is_split)) {
		scan_nursery_objects (ctx);
	} else {
		pin_objects_in_nursery (concurrent, ctx);
		if (check_nursery_objects_pinned && !sgen_minor_collector.is_split)
			sgen_check_nursery_objects_pinned (mode != COPY_OR_MARK_FROM_ROOTS_START_CONCURRENT);
	}

	major_collector.pin_objects (WORKERS_DISTRIBUTE_GRAY_QUEUE);
	if (old_next_pin_slot)
		*old_next_pin_slot = sgen_get_pinned_count ();

	TV_GETTIME (btv);
	time_major_pinning += TV_ELAPSED (atv, btv);
	SGEN_LOG (2, "Finding pinned pointers: %zd in %d usecs", sgen_get_pinned_count (), TV_ELAPSED (atv, btv));
	SGEN_LOG (4, "Start scan with %zd pinned objects", sgen_get_pinned_count ());

	major_collector.init_to_space ();

	/*
	 * The concurrent collector doesn't move objects, neither on
	 * the major heap nor in the nursery, so we can mark even
	 * before pinning has finished.  For the non-concurrent
	 * collector we start the workers after pinning.
	 */
	if (mode != COPY_OR_MARK_FROM_ROOTS_SERIAL) {
		SGEN_ASSERT (0, sgen_workers_all_done (), "Why are the workers not done when we start or finish a major collection?");
		sgen_workers_start_all_workers (object_ops);
		gray_queue_enable_redirect (WORKERS_DISTRIBUTE_GRAY_QUEUE);
	}

#ifdef SGEN_DEBUG_INTERNAL_ALLOC
	main_gc_thread = mono_native_thread_self ();
#endif

	sgen_client_collecting_major_2 ();

	TV_GETTIME (atv);
	time_major_scan_pinned += TV_ELAPSED (btv, atv);

	sgen_client_collecting_major_3 (&fin_ready_queue, &critical_fin_queue);

	/*
	 * FIXME: is this the right context?  It doesn't seem to contain a copy function
	 * unless we're concurrent.
	 */
	enqueue_scan_from_roots_jobs (heap_start, heap_end, object_ops);

	TV_GETTIME (btv);
	time_major_scan_roots += TV_ELAPSED (atv, btv);

	if (mode == COPY_OR_MARK_FROM_ROOTS_FINISH_CONCURRENT) {
		ScanJob *sj;

		/* Mod union card table */
		sj = (ScanJob*)sgen_thread_pool_job_alloc ("scan mod union cardtable", job_scan_major_mod_union_card_table, sizeof (ScanJob));
		sj->ops = object_ops;
		sgen_workers_enqueue_job (&sj->job);

		sj = (ScanJob*)sgen_thread_pool_job_alloc ("scan LOS mod union cardtable", job_scan_los_mod_union_card_table, sizeof (ScanJob));
		sj->ops = object_ops;
		sgen_workers_enqueue_job (&sj->job);

		TV_GETTIME (atv);
		time_major_scan_mod_union += TV_ELAPSED (btv, atv);
	}
}

static void
major_finish_copy_or_mark (void)
{
	if (!concurrent_collection_in_progress)
		return;

	/*
	 * Prepare the pin queue for the next collection.  Since pinning runs on the worker
	 * threads we must wait for the jobs to finish before we can reset it.
	 */
	sgen_workers_wait_for_jobs_finished ();
	sgen_finish_pinning ();

	sgen_pin_stats_reset ();

	if (do_concurrent_checks)
		sgen_debug_check_nursery_is_clean ();
}

static void
major_start_collection (gboolean concurrent, size_t *old_next_pin_slot)
{
	SgenObjectOperations *object_ops;

	binary_protocol_collection_begin (gc_stats.major_gc_count, GENERATION_OLD);

	current_collection_generation = GENERATION_OLD;
#ifndef DISABLE_PERFCOUNTERS
	mono_perfcounters->gc_collections1++;
#endif

	g_assert (sgen_section_gray_queue_is_empty (sgen_workers_get_distribute_section_gray_queue ()));

	sgen_cement_reset ();

	if (concurrent) {
		g_assert (major_collector.is_concurrent);
		concurrent_collection_in_progress = TRUE;

		object_ops = &major_collector.major_ops_concurrent_start;
	} else {
		object_ops = &major_collector.major_ops_serial;
	}

	reset_pinned_from_failed_allocation ();

	sgen_memgov_major_collection_start ();

	//count_ref_nonref_objs ();
	//consistency_check ();

	check_scan_starts ();

	degraded_mode = 0;
	SGEN_LOG (1, "Start major collection %d", gc_stats.major_gc_count);
	gc_stats.major_gc_count ++;

	if (major_collector.start_major_collection)
		major_collector.start_major_collection ();

	major_copy_or_mark_from_roots (old_next_pin_slot, concurrent ? COPY_OR_MARK_FROM_ROOTS_START_CONCURRENT : COPY_OR_MARK_FROM_ROOTS_SERIAL, FALSE, object_ops);
	major_finish_copy_or_mark ();
}

static void
major_finish_collection (const char *reason, size_t old_next_pin_slot, gboolean forced, gboolean scan_whole_nursery)
{
	ScannedObjectCounts counts;
	SgenObjectOperations *object_ops;
	TV_DECLARE (atv);
	TV_DECLARE (btv);

	TV_GETTIME (btv);

	if (concurrent_collection_in_progress) {
		object_ops = &major_collector.major_ops_concurrent_finish;

		major_copy_or_mark_from_roots (NULL, COPY_OR_MARK_FROM_ROOTS_FINISH_CONCURRENT, scan_whole_nursery, object_ops);

		major_finish_copy_or_mark ();

		sgen_workers_join ();

		SGEN_ASSERT (0, sgen_gray_object_queue_is_empty (&gray_queue), "Why is the gray queue not empty after workers have finished working?");

#ifdef SGEN_DEBUG_INTERNAL_ALLOC
		main_gc_thread = NULL;
#endif

		if (do_concurrent_checks)
			sgen_debug_check_nursery_is_clean ();
	} else {
		SGEN_ASSERT (0, !scan_whole_nursery, "scan_whole_nursery only applies to concurrent collections");
		object_ops = &major_collector.major_ops_serial;
	}

	/*
	 * The workers have stopped so we need to finish gray queue
	 * work that might result from finalization in the main GC
	 * thread.  Redirection must therefore be turned off.
	 */
	sgen_gray_object_queue_disable_alloc_prepare (&gray_queue);
	g_assert (sgen_section_gray_queue_is_empty (sgen_workers_get_distribute_section_gray_queue ()));

	/* all the objects in the heap */
	finish_gray_stack (GENERATION_OLD, CONTEXT_FROM_OBJECT_OPERATIONS (object_ops, &gray_queue));
	TV_GETTIME (atv);
	time_major_finish_gray_stack += TV_ELAPSED (btv, atv);

	SGEN_ASSERT (0, sgen_workers_all_done (), "Can't have workers working after joining");

	if (objects_pinned) {
		g_assert (!concurrent_collection_in_progress);

		/*
		 * This is slow, but we just OOM'd.
		 *
		 * See comment at `sgen_pin_queue_clear_discarded_entries` for how the pin
		 * queue is laid out at this point.
		 */
		sgen_pin_queue_clear_discarded_entries (nursery_section, old_next_pin_slot);
		/*
		 * We need to reestablish all pinned nursery objects in the pin queue
		 * because they're needed for fragment creation.  Unpinning happens by
		 * walking the whole queue, so it's not necessary to reestablish where major
		 * heap block pins are - all we care is that they're still in there
		 * somewhere.
		 */
		sgen_optimize_pin_queue ();
		sgen_find_section_pin_queue_start_end (nursery_section);
		objects_pinned = 0;
	}

	reset_heap_boundaries ();
	sgen_update_heap_boundaries ((mword)sgen_get_nursery_start (), (mword)sgen_get_nursery_end ());

	if (!concurrent_collection_in_progress) {
		/* walk the pin_queue, build up the fragment list of free memory, unmark
		 * pinned objects as we go, memzero() the empty fragments so they are ready for the
		 * next allocations.
		 */
		if (!sgen_build_nursery_fragments (nursery_section, NULL))
			degraded_mode = 1;

		/* prepare the pin queue for the next collection */
		sgen_finish_pinning ();

		/* Clear TLABs for all threads */
		sgen_clear_tlabs ();

		sgen_pin_stats_reset ();
	}

	sgen_cement_clear_below_threshold ();

	if (check_mark_bits_after_major_collection)
		sgen_check_heap_marked (concurrent_collection_in_progress);

	TV_GETTIME (btv);
	time_major_fragment_creation += TV_ELAPSED (atv, btv);

	binary_protocol_sweep_begin (GENERATION_OLD, !major_collector.sweeps_lazily);

	TV_GETTIME (atv);
	time_major_free_bigobjs += TV_ELAPSED (btv, atv);

	sgen_los_sweep ();

	TV_GETTIME (btv);
	time_major_los_sweep += TV_ELAPSED (atv, btv);

	major_collector.sweep ();

	binary_protocol_sweep_end (GENERATION_OLD, !major_collector.sweeps_lazily);

	TV_GETTIME (atv);
	time_major_sweep += TV_ELAPSED (btv, atv);

	sgen_debug_dump_heap ("major", gc_stats.major_gc_count - 1, reason);

	if (mono_gc_pending_finalizers ()) {
		SGEN_LOG (4, "Finalizer-thread wakeup");
		mono_gc_finalize_notify ();
	}

	g_assert (sgen_gray_object_queue_is_empty (&gray_queue));

	sgen_memgov_major_collection_end (forced);
	current_collection_generation = -1;

	memset (&counts, 0, sizeof (ScannedObjectCounts));
	major_collector.finish_major_collection (&counts);

	g_assert (sgen_section_gray_queue_is_empty (sgen_workers_get_distribute_section_gray_queue ()));

	SGEN_ASSERT (0, sgen_workers_all_done (), "Can't have workers working after major collection has finished");
	if (concurrent_collection_in_progress)
		concurrent_collection_in_progress = FALSE;

	check_scan_starts ();

	binary_protocol_flush_buffers (FALSE);

	//consistency_check ();

	binary_protocol_collection_end (gc_stats.major_gc_count - 1, GENERATION_OLD, counts.num_scanned_objects, counts.num_unique_scanned_objects);
}

static gboolean
major_do_collection (const char *reason, gboolean forced)
{
	TV_DECLARE (time_start);
	TV_DECLARE (time_end);
	size_t old_next_pin_slot;

	if (disable_major_collections)
		return FALSE;

	if (major_collector.get_and_reset_num_major_objects_marked) {
		long long num_marked = major_collector.get_and_reset_num_major_objects_marked ();
		g_assert (!num_marked);
	}

	/* world must be stopped already */
	TV_GETTIME (time_start);

	major_start_collection (FALSE, &old_next_pin_slot);
	major_finish_collection (reason, old_next_pin_slot, forced, FALSE);

	TV_GETTIME (time_end);
	gc_stats.major_gc_time += TV_ELAPSED (time_start, time_end);

	/* FIXME: also report this to the user, preferably in gc-end. */
	if (major_collector.get_and_reset_num_major_objects_marked)
		major_collector.get_and_reset_num_major_objects_marked ();

	return bytes_pinned_from_failed_allocation > 0;
}

static void
major_start_concurrent_collection (const char *reason)
{
	TV_DECLARE (time_start);
	TV_DECLARE (time_end);
	long long num_objects_marked;

	if (disable_major_collections)
		return;

	TV_GETTIME (time_start);
	SGEN_TV_GETTIME (time_major_conc_collection_start);

	num_objects_marked = major_collector.get_and_reset_num_major_objects_marked ();
	g_assert (num_objects_marked == 0);

	binary_protocol_concurrent_start ();

	// FIXME: store reason and pass it when finishing
	major_start_collection (TRUE, NULL);

	gray_queue_redirect (&gray_queue);

	num_objects_marked = major_collector.get_and_reset_num_major_objects_marked ();

	TV_GETTIME (time_end);
	gc_stats.major_gc_time += TV_ELAPSED (time_start, time_end);

	current_collection_generation = -1;
}

/*
 * Returns whether the major collection has finished.
 */
static gboolean
major_should_finish_concurrent_collection (void)
{
	SGEN_ASSERT (0, sgen_gray_object_queue_is_empty (&gray_queue), "Why is the gray queue not empty before we have started doing anything?");
	return sgen_workers_all_done ();
}

static void
major_update_concurrent_collection (void)
{
	TV_DECLARE (total_start);
	TV_DECLARE (total_end);

	TV_GETTIME (total_start);

	binary_protocol_concurrent_update ();

	major_collector.update_cardtable_mod_union ();
	sgen_los_update_cardtable_mod_union ();

	TV_GETTIME (total_end);
	gc_stats.major_gc_time += TV_ELAPSED (total_start, total_end);
}

static void
major_finish_concurrent_collection (gboolean forced)
{
	TV_DECLARE (total_start);
	TV_DECLARE (total_end);
	gboolean late_pinned;
	SgenGrayQueue unpin_queue;
	memset (&unpin_queue, 0, sizeof (unpin_queue));

	TV_GETTIME (total_start);

	binary_protocol_concurrent_finish ();

	/*
	 * The major collector can add global remsets which are processed in the finishing
	 * nursery collection, below.  That implies that the workers must have finished
	 * marking before the nursery collection is allowed to run, otherwise we might miss
	 * some remsets.
	 */
	sgen_workers_wait ();

	SGEN_TV_GETTIME (time_major_conc_collection_end);
	gc_stats.major_gc_time_concurrent += SGEN_TV_ELAPSED (time_major_conc_collection_start, time_major_conc_collection_end);

	major_collector.update_cardtable_mod_union ();
	sgen_los_update_cardtable_mod_union ();

	late_pinned = collect_nursery (&unpin_queue, TRUE);

	if (mod_union_consistency_check)
		sgen_check_mod_union_consistency ();

	current_collection_generation = GENERATION_OLD;
	major_finish_collection ("finishing", -1, forced, late_pinned);

	if (whole_heap_check_before_collection)
		sgen_check_whole_heap (FALSE);

	unpin_objects_from_queue (&unpin_queue);
	sgen_gray_object_queue_deinit (&unpin_queue);

	TV_GETTIME (total_end);
	gc_stats.major_gc_time += TV_ELAPSED (total_start, total_end) - TV_ELAPSED (last_minor_collection_start_tv, last_minor_collection_end_tv);

	current_collection_generation = -1;
}

/*
 * Ensure an allocation request for @size will succeed by freeing enough memory.
 *
 * LOCKING: The GC lock MUST be held.
 */
void
sgen_ensure_free_space (size_t size)
{
	int generation_to_collect = -1;
	const char *reason = NULL;

	if (size > SGEN_MAX_SMALL_OBJ_SIZE) {
		if (sgen_need_major_collection (size)) {
			reason = "LOS overflow";
			generation_to_collect = GENERATION_OLD;
		}
	} else {
		if (degraded_mode) {
			if (sgen_need_major_collection (size)) {
				reason = "Degraded mode overflow";
				generation_to_collect = GENERATION_OLD;
			}
		} else if (sgen_need_major_collection (size)) {
			reason = "Minor allowance";
			generation_to_collect = GENERATION_OLD;
		} else {
			generation_to_collect = GENERATION_NURSERY;
			reason = "Nursery full";                        
		}
	}

	if (generation_to_collect == -1) {
		if (concurrent_collection_in_progress && sgen_workers_all_done ()) {
			generation_to_collect = GENERATION_OLD;
			reason = "Finish concurrent collection";
		}
	}

	if (generation_to_collect == -1)
		return;
	sgen_perform_collection (size, generation_to_collect, reason, FALSE);
}

/*
 * LOCKING: Assumes the GC lock is held.
 */
void
sgen_perform_collection (size_t requested_size, int generation_to_collect, const char *reason, gboolean wait_to_finish)
{
	TV_DECLARE (gc_start);
	TV_DECLARE (gc_end);
	TV_DECLARE (gc_total_start);
	TV_DECLARE (gc_total_end);
	GGTimingInfo infos [2];
	int overflow_generation_to_collect = -1;
	int oldest_generation_collected = generation_to_collect;
	const char *overflow_reason = NULL;

	binary_protocol_collection_requested (generation_to_collect, requested_size, wait_to_finish ? 1 : 0);

	SGEN_ASSERT (0, generation_to_collect == GENERATION_NURSERY || generation_to_collect == GENERATION_OLD, "What generation is this?");

	TV_GETTIME (gc_start);

	sgen_stop_world (generation_to_collect);

	TV_GETTIME (gc_total_start);

	if (concurrent_collection_in_progress) {
		/*
		 * We update the concurrent collection.  If it finished, we're done.  If
		 * not, and we've been asked to do a nursery collection, we do that.
		 */
		gboolean finish = major_should_finish_concurrent_collection () || (wait_to_finish && generation_to_collect == GENERATION_OLD);

		if (finish) {
			major_finish_concurrent_collection (wait_to_finish);
			oldest_generation_collected = GENERATION_OLD;
		} else {
			sgen_workers_signal_start_nursery_collection_and_wait ();

			major_update_concurrent_collection ();
			if (generation_to_collect == GENERATION_NURSERY)
				collect_nursery (NULL, FALSE);

			sgen_workers_signal_finish_nursery_collection ();
		}

		goto done;
	}

	/*
	 * If we've been asked to do a major collection, and the major collector wants to
	 * run synchronously (to evacuate), we set the flag to do that.
	 */
	if (generation_to_collect == GENERATION_OLD &&
			allow_synchronous_major &&
			major_collector.want_synchronous_collection &&
			*major_collector.want_synchronous_collection) {
		wait_to_finish = TRUE;
	}

	SGEN_ASSERT (0, !concurrent_collection_in_progress, "Why did this not get handled above?");

	/*
	 * There's no concurrent collection in progress.  Collect the generation we're asked
	 * to collect.  If the major collector is concurrent and we're not forced to wait,
	 * start a concurrent collection.
	 */
	// FIXME: extract overflow reason
	if (generation_to_collect == GENERATION_NURSERY) {
		if (collect_nursery (NULL, FALSE)) {
			overflow_generation_to_collect = GENERATION_OLD;
			overflow_reason = "Minor overflow";
		}
	} else {
		if (major_collector.is_concurrent && !wait_to_finish) {
			collect_nursery (NULL, FALSE);
			major_start_concurrent_collection (reason);
			// FIXME: set infos[0] properly
			goto done;
		}

		if (major_do_collection (reason, wait_to_finish)) {
			overflow_generation_to_collect = GENERATION_NURSERY;
			overflow_reason = "Excessive pinning";
		}
	}

	TV_GETTIME (gc_end);

	memset (infos, 0, sizeof (infos));
	infos [0].generation = generation_to_collect;
	infos [0].reason = reason;
	infos [0].is_overflow = FALSE;
	infos [1].generation = -1;
	infos [0].total_time = SGEN_TV_ELAPSED (gc_start, gc_end);

	SGEN_ASSERT (0, !concurrent_collection_in_progress, "Why did this not get handled above?");

	if (overflow_generation_to_collect != -1) {
		/*
		 * We need to do an overflow collection, either because we ran out of memory
		 * or the nursery is fully pinned.
		 */

		infos [1].generation = overflow_generation_to_collect;
		infos [1].reason = overflow_reason;
		infos [1].is_overflow = TRUE;
		infos [1].total_time = gc_end;

		if (overflow_generation_to_collect == GENERATION_NURSERY)
			collect_nursery (NULL, FALSE);
		else
			major_do_collection (overflow_reason, wait_to_finish);

		TV_GETTIME (gc_end);
		infos [1].total_time = SGEN_TV_ELAPSED (infos [1].total_time, gc_end);

		oldest_generation_collected = MAX (oldest_generation_collected, overflow_generation_to_collect);
	}

	SGEN_LOG (2, "Heap size: %lu, LOS size: %lu", (unsigned long)mono_gc_get_heap_size (), (unsigned long)los_memory_usage);

	/* this also sets the proper pointers for the next allocation */
	if (generation_to_collect == GENERATION_NURSERY && !sgen_can_alloc_size (requested_size)) {
		/* TypeBuilder and MonoMethod are killing mcs with fragmentation */
		SGEN_LOG (1, "nursery collection didn't find enough room for %zd alloc (%zd pinned)", requested_size, sgen_get_pinned_count ());
		sgen_dump_pin_queue ();
		degraded_mode = 1;
	}

 done:
	g_assert (sgen_gray_object_queue_is_empty (&gray_queue));

	TV_GETTIME (gc_total_end);
	time_max = MAX (time_max, TV_ELAPSED (gc_total_start, gc_total_end));

	sgen_restart_world (oldest_generation_collected, infos);
}

/*
 * ######################################################################
 * ########  Memory allocation from the OS
 * ######################################################################
 * This section of code deals with getting memory from the OS and
 * allocating memory for GC-internal data structures.
 * Internal memory can be handled with a freelist for small objects.
 */

/*
 * Debug reporting.
 */
G_GNUC_UNUSED static void
report_internal_mem_usage (void)
{
	printf ("Internal memory usage:\n");
	sgen_report_internal_mem_usage ();
	printf ("Pinned memory usage:\n");
	major_collector.report_pinned_memory_usage ();
}

/*
 * ######################################################################
 * ########  Finalization support
 * ######################################################################
 */

/*
 * If the object has been forwarded it means it's still referenced from a root. 
 * If it is pinned it's still alive as well.
 * A LOS object is only alive if we have pinned it.
 * Return TRUE if @obj is ready to be finalized.
 */
static inline gboolean
sgen_is_object_alive (void *object)
{
	if (ptr_in_nursery (object))
		return sgen_nursery_is_object_alive (object);

	return sgen_major_is_object_alive (object);
}

/*
 * This function returns true if @object is either alive and belongs to the
 * current collection - major collections are full heap, so old gen objects
 * are never alive during a minor collection.
 */
static inline int
sgen_is_object_alive_and_on_current_collection (char *object)
{
	if (ptr_in_nursery (object))
		return sgen_nursery_is_object_alive (object);

	if (current_collection_generation == GENERATION_NURSERY)
		return FALSE;

	return sgen_major_is_object_alive (object);
}


gboolean
sgen_gc_is_object_ready_for_finalization (void *object)
{
	return !sgen_is_object_alive (object);
}

void
sgen_queue_finalization_entry (GCObject *obj)
{
	gboolean critical = sgen_client_object_has_critical_finalizer (obj);

	sgen_pointer_queue_add (critical ? &critical_fin_queue : &fin_ready_queue, obj);

	sgen_client_object_queued_for_finalization (obj);
}

gboolean
sgen_object_is_live (void *obj)
{
	return sgen_is_object_alive_and_on_current_collection (obj);
}

/*
 * `System.GC.WaitForPendingFinalizers` first checks `sgen_have_pending_finalizers()` to
 * determine whether it can exit quickly.  The latter must therefore only return FALSE if
 * all finalizers have really finished running.
 *
 * `sgen_gc_invoke_finalizers()` first dequeues a finalizable object, and then finalizes it.
 * This means that just checking whether the queues are empty leaves the possibility that an
 * object might have been dequeued but not yet finalized.  That's why we need the additional
 * flag `pending_unqueued_finalizer`.
 */

static volatile gboolean pending_unqueued_finalizer = FALSE;

int
mono_gc_invoke_finalizers (void)
{
	int count = 0;

	g_assert (!pending_unqueued_finalizer);

	/* FIXME: batch to reduce lock contention */
	while (mono_gc_pending_finalizers ()) {
		void *obj;

		LOCK_GC;

		/*
		 * We need to set `pending_unqueued_finalizer` before dequeing the
		 * finalizable object.
		 */
		if (!sgen_pointer_queue_is_empty (&fin_ready_queue)) {
			pending_unqueued_finalizer = TRUE;
			mono_memory_write_barrier ();
			obj = sgen_pointer_queue_pop (&fin_ready_queue);
		} else if (!sgen_pointer_queue_is_empty (&critical_fin_queue)) {
			pending_unqueued_finalizer = TRUE;
			mono_memory_write_barrier ();
			obj = sgen_pointer_queue_pop (&critical_fin_queue);
		} else {
			obj = NULL;
		}

		if (obj)
			SGEN_LOG (7, "Finalizing object %p (%s)", obj, sgen_client_object_safe_name (obj));

		UNLOCK_GC;

		if (!obj)
			break;

		count++;
		/* the object is on the stack so it is pinned */
		/*g_print ("Calling finalizer for object: %p (%s)\n", obj, sgen_client_object_safe_name (obj));*/
		mono_gc_run_finalize (obj, NULL);
	}

	if (pending_unqueued_finalizer) {
		mono_memory_write_barrier ();
		pending_unqueued_finalizer = FALSE;
	}

	return count;
}

gboolean
mono_gc_pending_finalizers (void)
{
	return pending_unqueued_finalizer || !sgen_pointer_queue_is_empty (&fin_ready_queue) || !sgen_pointer_queue_is_empty (&critical_fin_queue);
}

/*
 * ######################################################################
 * ########  registered roots support
 * ######################################################################
 */

/*
 * We do not coalesce roots.
 */
static int
mono_gc_register_root_inner (char *start, size_t size, void *descr, int root_type)
{
	RootRecord new_root;
	int i;
	LOCK_GC;
	for (i = 0; i < ROOT_TYPE_NUM; ++i) {
		RootRecord *root = sgen_hash_table_lookup (&roots_hash [i], start);
		/* we allow changing the size and the descriptor (for thread statics etc) */
		if (root) {
			size_t old_size = root->end_root - start;
			root->end_root = start + size;
			g_assert (((root->root_desc != 0) && (descr != NULL)) ||
					  ((root->root_desc == 0) && (descr == NULL)));
			root->root_desc = (mword)descr;
			roots_size += size;
			roots_size -= old_size;
			UNLOCK_GC;
			return TRUE;
		}
	}

	new_root.end_root = start + size;
	new_root.root_desc = (mword)descr;

	sgen_hash_table_replace (&roots_hash [root_type], start, &new_root, NULL);
	roots_size += size;

	SGEN_LOG (3, "Added root for range: %p-%p, descr: %p  (%d/%d bytes)", start, new_root.end_root, descr, (int)size, (int)roots_size);

	UNLOCK_GC;
	return TRUE;
}

int
mono_gc_register_root (char *start, size_t size, void *descr)
{
	return mono_gc_register_root_inner (start, size, descr, descr ? ROOT_TYPE_NORMAL : ROOT_TYPE_PINNED);
}

int
mono_gc_register_root_wbarrier (char *start, size_t size, void *descr)
{
	return mono_gc_register_root_inner (start, size, descr, ROOT_TYPE_WBARRIER);
}

void
mono_gc_deregister_root (char* addr)
{
	int root_type;
	RootRecord root;

	LOCK_GC;
	for (root_type = 0; root_type < ROOT_TYPE_NUM; ++root_type) {
		if (sgen_hash_table_remove (&roots_hash [root_type], addr, &root))
			roots_size -= (root.end_root - addr);
	}
	UNLOCK_GC;
}

/*
 * ######################################################################
 * ########  Thread handling (stop/start code)
 * ######################################################################
 */

unsigned int sgen_global_stop_count = 0;

int
sgen_get_current_collection_generation (void)
{
	return current_collection_generation;
}

void
mono_gc_set_gc_callbacks (MonoGCCallbacks *callbacks)
{
	gc_callbacks = *callbacks;
}

MonoGCCallbacks *
mono_gc_get_gc_callbacks ()
{
	return &gc_callbacks;
}

/* Variables holding start/end nursery so it won't have to be passed at every call */
static void *scan_area_arg_start, *scan_area_arg_end;

void
mono_gc_conservatively_scan_area (void *start, void *end)
{
	conservatively_pin_objects_from (start, end, scan_area_arg_start, scan_area_arg_end, PIN_TYPE_STACK);
}

void*
mono_gc_scan_object (void *obj, void *gc_data)
{
	ScanCopyContext *ctx = gc_data;
	ctx->ops->copy_or_mark_object (&obj, ctx->queue);
	return obj;
}

/*
 * Mark from thread stacks and registers.
 */
static void
scan_thread_data (void *start_nursery, void *end_nursery, gboolean precise, ScanCopyContext ctx)
{
	SgenThreadInfo *info;

	scan_area_arg_start = start_nursery;
	scan_area_arg_end = end_nursery;

	FOREACH_THREAD (info) {
		if (info->skip) {
			SGEN_LOG (3, "Skipping dead thread %p, range: %p-%p, size: %td", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start);
			continue;
		}
		if (info->gc_disabled) {
			SGEN_LOG (3, "GC disabled for thread %p, range: %p-%p, size: %td", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start);
			continue;
		}
		if (!mono_thread_info_is_live (info)) {
			SGEN_LOG (3, "Skipping non-running thread %p, range: %p-%p, size: %td (state %x)", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start, info->info.thread_state);
			continue;
		}
		g_assert (info->suspend_done);
		SGEN_LOG (3, "Scanning thread %p, range: %p-%p, size: %td, pinned=%zd", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start, sgen_get_pinned_count ());
		if (gc_callbacks.thread_mark_func && !conservative_stack_mark) {
			gc_callbacks.thread_mark_func (info->runtime_data, info->stack_start, info->stack_end, precise, &ctx);
		} else if (!precise) {
			if (!conservative_stack_mark) {
				fprintf (stderr, "Precise stack mark not supported - disabling.\n");
				conservative_stack_mark = TRUE;
			}
			conservatively_pin_objects_from (info->stack_start, info->stack_end, start_nursery, end_nursery, PIN_TYPE_STACK);
		}

		if (!precise) {
#ifdef USE_MONO_CTX
			conservatively_pin_objects_from ((void**)&info->ctx, (void**)&info->ctx + ARCH_NUM_REGS,
				start_nursery, end_nursery, PIN_TYPE_STACK);
#else
			conservatively_pin_objects_from ((void**)&info->regs, (void**)&info->regs + ARCH_NUM_REGS,
					start_nursery, end_nursery, PIN_TYPE_STACK);
#endif
		}
	} END_FOREACH_THREAD
}

void*
sgen_thread_register (SgenThreadInfo* info, void *addr)
{
	size_t stsize = 0;
	guint8 *staddr = NULL;

#ifndef HAVE_KW_THREAD
	info->tlab_start = info->tlab_next = info->tlab_temp_end = info->tlab_real_end = NULL;

	g_assert (!mono_native_tls_get_value (thread_info_key));
	mono_native_tls_set_value (thread_info_key, info);
#else
	sgen_thread_info = info;
#endif

#ifdef SGEN_POSIX_STW
	info->stop_count = -1;
	info->signal = 0;
#endif
	info->skip = 0;
	info->stack_start = NULL;
	info->stopped_ip = NULL;
	info->stopped_domain = NULL;
#ifdef USE_MONO_CTX
	memset (&info->ctx, 0, sizeof (MonoContext));
#else
	memset (&info->regs, 0, sizeof (info->regs));
#endif

	sgen_init_tlab_info (info);

	binary_protocol_thread_register ((gpointer)mono_thread_info_get_tid (info));

	/* On win32, stack_start_limit should be 0, since the stack can grow dynamically */
	mono_thread_info_get_stack_bounds (&staddr, &stsize);
	if (staddr) {
#ifndef HOST_WIN32
		info->stack_start_limit = staddr;
#endif
		info->stack_end = staddr + stsize;
	} else {
		gsize stack_bottom = (gsize)addr;
		stack_bottom += 4095;
		stack_bottom &= ~4095;
		info->stack_end = (char*)stack_bottom;
	}

#ifdef HAVE_KW_THREAD
	stack_end = info->stack_end;
#endif

	SGEN_LOG (3, "registered thread %p (%p) stack end %p", info, (gpointer)mono_thread_info_get_tid (info), info->stack_end);

	if (gc_callbacks.thread_attach_func)
		info->runtime_data = gc_callbacks.thread_attach_func ();
	return info;
}

void
sgen_thread_unregister (SgenThreadInfo *p)
{
	MonoNativeThreadId tid;

	tid = mono_thread_info_get_tid (p);
	binary_protocol_thread_unregister ((gpointer)tid);
	SGEN_LOG (3, "unregister thread %p (%p)", p, (gpointer)tid);

#ifndef HAVE_KW_THREAD
	mono_native_tls_set_value (thread_info_key, NULL);
#else
	sgen_thread_info = NULL;
#endif

	if (p->info.runtime_thread)
		mono_threads_add_joinable_thread ((gpointer)tid);

	if (gc_callbacks.thread_detach_func) {
		gc_callbacks.thread_detach_func (p->runtime_data);
		p->runtime_data = NULL;
	}
}


void
sgen_thread_attach (SgenThreadInfo *info)
{
	LOCK_GC;
	/*this is odd, can we get attached before the gc is inited?*/
	init_stats ();
	UNLOCK_GC;
	
	if (gc_callbacks.thread_attach_func && !info->runtime_data)
		info->runtime_data = gc_callbacks.thread_attach_func ();
}
gboolean
mono_gc_register_thread (void *baseptr)
{
	return mono_thread_info_attach (baseptr) != NULL;
}

/*
 * mono_gc_set_stack_end:
 *
 *   Set the end of the current threads stack to STACK_END. The stack space between 
 * STACK_END and the real end of the threads stack will not be scanned during collections.
 */
void
mono_gc_set_stack_end (void *stack_end)
{
	SgenThreadInfo *info;

	LOCK_GC;
	info = mono_thread_info_current ();
	if (info) {
		g_assert (stack_end < info->stack_end);
		info->stack_end = stack_end;
	}
	UNLOCK_GC;
}

#if USE_PTHREAD_INTERCEPT


int
mono_gc_pthread_create (pthread_t *new_thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
	return pthread_create (new_thread, attr, start_routine, arg);
}

int
mono_gc_pthread_join (pthread_t thread, void **retval)
{
	return pthread_join (thread, retval);
}

int
mono_gc_pthread_detach (pthread_t thread)
{
	return pthread_detach (thread);
}

void
mono_gc_pthread_exit (void *retval) 
{
	mono_thread_info_detach ();
	pthread_exit (retval);
	g_assert_not_reached ();
}

#endif /* USE_PTHREAD_INTERCEPT */

/*
 * ######################################################################
 * ########  Write barriers
 * ######################################################################
 */

/*
 * Note: the write barriers first do the needed GC work and then do the actual store:
 * this way the value is visible to the conservative GC scan after the write barrier
 * itself. If a GC interrupts the barrier in the middle, value will be kept alive by
 * the conservative scan, otherwise by the remembered set scan.
 */
void
mono_gc_wbarrier_set_field (MonoObject *obj, gpointer field_ptr, MonoObject* value)
{
	HEAVY_STAT (++stat_wbarrier_set_field);
	if (ptr_in_nursery (field_ptr)) {
		*(void**)field_ptr = value;
		return;
	}
	SGEN_LOG (8, "Adding remset at %p", field_ptr);
	if (value)
		binary_protocol_wbarrier (field_ptr, value, value->vtable);

	remset.wbarrier_set_field (obj, field_ptr, value);
}

void
mono_gc_wbarrier_arrayref_copy (gpointer dest_ptr, gpointer src_ptr, int count)
{
	HEAVY_STAT (++stat_wbarrier_arrayref_copy);
	/*This check can be done without taking a lock since dest_ptr array is pinned*/
	if (ptr_in_nursery (dest_ptr) || count <= 0) {
		mono_gc_memmove_aligned (dest_ptr, src_ptr, count * sizeof (gpointer));
		return;
	}

#ifdef SGEN_HEAVY_BINARY_PROTOCOL
	if (binary_protocol_is_heavy_enabled ()) {
		int i;
		for (i = 0; i < count; ++i) {
			gpointer dest = (gpointer*)dest_ptr + i;
			gpointer obj = *((gpointer*)src_ptr + i);
			if (obj)
				binary_protocol_wbarrier (dest, obj, (gpointer)LOAD_VTABLE (obj));
		}
	}
#endif

	remset.wbarrier_arrayref_copy (dest_ptr, src_ptr, count);
}

void
mono_gc_wbarrier_generic_nostore (gpointer ptr)
{
	gpointer obj;

	HEAVY_STAT (++stat_wbarrier_generic_store);

	sgen_client_wbarrier_generic_nostore_check (ptr);

	obj = *(gpointer*)ptr;
	if (obj)
		binary_protocol_wbarrier (ptr, obj, (gpointer)LOAD_VTABLE (obj));

	/*
	 * We need to record old->old pointer locations for the
	 * concurrent collector.
	 */
	if (!ptr_in_nursery (obj) && !concurrent_collection_in_progress) {
		SGEN_LOG (8, "Skipping remset at %p", ptr);
		return;
	}

	SGEN_LOG (8, "Adding remset at %p", ptr);

	remset.wbarrier_generic_nostore (ptr);
}

void
mono_gc_wbarrier_generic_store (gpointer ptr, MonoObject* value)
{
	SGEN_LOG (8, "Wbarrier store at %p to %p (%s)", ptr, value, value ? sgen_client_object_safe_name (value) : "null");
	SGEN_UPDATE_REFERENCE_ALLOW_NULL (ptr, value);
	if (ptr_in_nursery (value))
		mono_gc_wbarrier_generic_nostore (ptr);
	sgen_dummy_use (value);
}

/* Same as mono_gc_wbarrier_generic_store () but performs the store
 * as an atomic operation with release semantics.
 */
void
mono_gc_wbarrier_generic_store_atomic (gpointer ptr, MonoObject *value)
{
	HEAVY_STAT (++stat_wbarrier_generic_store_atomic);

	SGEN_LOG (8, "Wbarrier atomic store at %p to %p (%s)", ptr, value, value ? sgen_client_object_safe_name (value) : "null");

	InterlockedWritePointer (ptr, value);

	if (ptr_in_nursery (value))
		mono_gc_wbarrier_generic_nostore (ptr);

	sgen_dummy_use (value);
}

void mono_gc_wbarrier_value_copy_bitmap (gpointer _dest, gpointer _src, int size, unsigned bitmap)
{
	GCObject **dest = _dest;
	GCObject **src = _src;

	while (size) {
		if (bitmap & 0x1)
			mono_gc_wbarrier_generic_store (dest, *src);
		else
			SGEN_UPDATE_REFERENCE_ALLOW_NULL (dest, *src);
		++src;
		++dest;
		size -= SIZEOF_VOID_P;
		bitmap >>= 1;
	}
}

/*
 * ######################################################################
 * ########  Other mono public interface functions.
 * ######################################################################
 */

void
mono_gc_collect (int generation)
{
	LOCK_GC;
	if (generation > 1)
		generation = 1;
	sgen_perform_collection (0, generation, "user request", TRUE);
	UNLOCK_GC;
}

int
mono_gc_collection_count (int generation)
{
	if (generation == 0)
		return gc_stats.minor_gc_count;
	return gc_stats.major_gc_count;
}

int64_t
mono_gc_get_used_size (void)
{
	gint64 tot = 0;
	LOCK_GC;
	tot = los_memory_usage;
	tot += nursery_section->next_data - nursery_section->data;
	tot += major_collector.get_used_size ();
	/* FIXME: account for pinned objects */
	UNLOCK_GC;
	return tot;
}

int
mono_gc_get_los_limit (void)
{
	return MAX_SMALL_OBJ_SIZE;
}

void
mono_gc_weak_link_add (void **link_addr, MonoObject *obj, gboolean track)
{
	sgen_register_disappearing_link (obj, link_addr, track, FALSE);
}

void
mono_gc_weak_link_remove (void **link_addr, gboolean track)
{
	sgen_register_disappearing_link (NULL, link_addr, track, FALSE);
}

MonoObject*
mono_gc_weak_link_get (void **link_addr)
{
	void * volatile *link_addr_volatile;
	void *ptr;
	MonoObject *obj;
 retry:
	link_addr_volatile = link_addr;
	ptr = (void*)*link_addr_volatile;
	/*
	 * At this point we have a hidden pointer.  If the GC runs
	 * here, it will not recognize the hidden pointer as a
	 * reference, and if the object behind it is not referenced
	 * elsewhere, it will be freed.  Once the world is restarted
	 * we reveal the pointer, giving us a pointer to a freed
	 * object.  To make sure we don't return it, we load the
	 * hidden pointer again.  If it's still the same, we can be
	 * sure the object reference is valid.
	 */
	if (ptr)
		obj = (MonoObject*) REVEAL_POINTER (ptr);
	else
		return NULL;

	mono_memory_barrier ();

	/*
	 * During the second bridge processing step the world is
	 * running again.  That step processes all weak links once
	 * more to null those that refer to dead objects.  Before that
	 * is completed, those links must not be followed, so we
	 * conservatively wait for bridge processing when any weak
	 * link is dereferenced.
	 */
	if (G_UNLIKELY (bridge_processing_in_progress))
		mono_gc_wait_for_bridge_processing ();

	if ((void*)*link_addr_volatile != ptr)
		goto retry;

	return obj;
}

gboolean
mono_gc_set_allow_synchronous_major (gboolean flag)
{
	if (!major_collector.is_concurrent)
		return flag;

	allow_synchronous_major = flag;
	return TRUE;
}

void*
mono_gc_invoke_with_gc_lock (MonoGCLockedCallbackFunc func, void *data)
{
	void *result;
	LOCK_INTERRUPTION;
	result = func (data);
	UNLOCK_INTERRUPTION;
	return result;
}

gboolean
mono_gc_is_gc_thread (void)
{
	gboolean result;
	LOCK_GC;
	result = mono_thread_info_current () != NULL;
	UNLOCK_GC;
	return result;
}

void
sgen_env_var_error (const char *env_var, const char *fallback, const char *description_format, ...)
{
	va_list ap;

	va_start (ap, description_format);

	fprintf (stderr, "Warning: In environment variable `%s': ", env_var);
	vfprintf (stderr, description_format, ap);
	if (fallback)
		fprintf (stderr, " - %s", fallback);
	fprintf (stderr, "\n");

	va_end (ap);
}

static gboolean
parse_double_in_interval (const char *env_var, const char *opt_name, const char *opt, double min, double max, double *result)
{
	char *endptr;
	double val = strtod (opt, &endptr);
	if (endptr == opt) {
		sgen_env_var_error (env_var, "Using default value.", "`%s` must be a number.", opt_name);
		return FALSE;
	}
	else if (val < min || val > max) {
		sgen_env_var_error (env_var, "Using default value.", "`%s` must be between %.2f - %.2f.", opt_name, min, max);
		return FALSE;
	}
	*result = val;
	return TRUE;
}

void
mono_gc_base_init (void)
{
	const char *env;
	char **opts, **ptr;
	char *major_collector_opt = NULL;
	char *minor_collector_opt = NULL;
	size_t max_heap = 0;
	size_t soft_limit = 0;
	int result;
	int dummy;
	gboolean debug_print_allowance = FALSE;
	double allowance_ratio = 0, save_target = 0;
	gboolean cement_enabled = TRUE;

	mono_counters_init ();

	do {
		result = InterlockedCompareExchange (&gc_initialized, -1, 0);
		switch (result) {
		case 1:
			/* already inited */
			return;
		case -1:
			/* being inited by another thread */
			g_usleep (1000);
			break;
		case 0:
			/* we will init it */
			break;
		default:
			g_assert_not_reached ();
		}
	} while (result != 0);

	SGEN_TV_GETTIME (sgen_init_timestamp);

	LOCK_INIT (gc_mutex);

	pagesize = mono_pagesize ();
	gc_debug_file = stderr;

	LOCK_INIT (sgen_interruption_mutex);

	if ((env = g_getenv (MONO_GC_PARAMS_NAME))) {
		opts = g_strsplit (env, ",", -1);
		for (ptr = opts; *ptr; ++ptr) {
			char *opt = *ptr;
			if (g_str_has_prefix (opt, "major=")) {
				opt = strchr (opt, '=') + 1;
				major_collector_opt = g_strdup (opt);
			} else if (g_str_has_prefix (opt, "minor=")) {
				opt = strchr (opt, '=') + 1;
				minor_collector_opt = g_strdup (opt);
			}
		}
	} else {
		opts = NULL;
	}

	init_stats ();
	sgen_init_internal_allocator ();
	sgen_init_nursery_allocator ();
	sgen_init_fin_weak_hash ();
	sgen_init_hash_table ();
	sgen_init_descriptors ();
	sgen_init_gray_queues ();
	sgen_init_allocator ();

	sgen_register_fixed_internal_mem_type (INTERNAL_MEM_SECTION, SGEN_SIZEOF_GC_MEM_SECTION);
	sgen_register_fixed_internal_mem_type (INTERNAL_MEM_GRAY_QUEUE, sizeof (GrayQueueSection));

	sgen_client_init ();

#ifndef HAVE_KW_THREAD
	mono_native_tls_alloc (&thread_info_key, NULL);
#if defined(__APPLE__) || defined (HOST_WIN32)
	/* 
	 * CEE_MONO_TLS requires the tls offset, not the key, so the code below only works on darwin,
	 * where the two are the same.
	 */
	mono_tls_key_set_offset (TLS_KEY_SGEN_THREAD_INFO, thread_info_key);
#endif
#else
	{
		int tls_offset = -1;
		MONO_THREAD_VAR_OFFSET (sgen_thread_info, tls_offset);
		mono_tls_key_set_offset (TLS_KEY_SGEN_THREAD_INFO, tls_offset);
	}
#endif

	/*
	 * This needs to happen before any internal allocations because
	 * it inits the small id which is required for hazard pointer
	 * operations.
	 */
	sgen_os_init ();

	mono_thread_info_attach (&dummy);

	if (!minor_collector_opt) {
		sgen_simple_nursery_init (&sgen_minor_collector);
	} else {
		if (!strcmp (minor_collector_opt, "simple")) {
		use_simple_nursery:
			sgen_simple_nursery_init (&sgen_minor_collector);
		} else if (!strcmp (minor_collector_opt, "split")) {
			sgen_split_nursery_init (&sgen_minor_collector);
		} else {
			sgen_env_var_error (MONO_GC_PARAMS_NAME, "Using `simple` instead.", "Unknown minor collector `%s'.", minor_collector_opt);
			goto use_simple_nursery;
		}
	}

	if (!major_collector_opt || !strcmp (major_collector_opt, "marksweep")) {
	use_marksweep_major:
		sgen_marksweep_init (&major_collector);
	} else if (!major_collector_opt || !strcmp (major_collector_opt, "marksweep-conc")) {
		sgen_marksweep_conc_init (&major_collector);
	} else {
		sgen_env_var_error (MONO_GC_PARAMS_NAME, "Using `marksweep` instead.", "Unknown major collector `%s'.", major_collector_opt);
		goto use_marksweep_major;
	}

	///* Keep this the default for now */
	/* Precise marking is broken on all supported targets. Disable until fixed. */
	conservative_stack_mark = TRUE;

	sgen_nursery_size = DEFAULT_NURSERY_SIZE;

	if (major_collector.is_concurrent)
		cement_enabled = FALSE;

	if (opts) {
		gboolean usage_printed = FALSE;

		for (ptr = opts; *ptr; ++ptr) {
			char *opt = *ptr;
			if (!strcmp (opt, ""))
				continue;
			if (g_str_has_prefix (opt, "major="))
				continue;
			if (g_str_has_prefix (opt, "minor="))
				continue;
			if (g_str_has_prefix (opt, "max-heap-size=")) {
				size_t max_heap_candidate = 0;
				opt = strchr (opt, '=') + 1;
				if (*opt && mono_gc_parse_environment_string_extract_number (opt, &max_heap_candidate)) {
					max_heap = (max_heap_candidate + mono_pagesize () - 1) & ~(size_t)(mono_pagesize () - 1);
					if (max_heap != max_heap_candidate)
						sgen_env_var_error (MONO_GC_PARAMS_NAME, "Rounding up.", "`max-heap-size` size must be a multiple of %d.", mono_pagesize ());
				} else {
					sgen_env_var_error (MONO_GC_PARAMS_NAME, NULL, "`max-heap-size` must be an integer.");
				}
				continue;
			}
			if (g_str_has_prefix (opt, "soft-heap-limit=")) {
				opt = strchr (opt, '=') + 1;
				if (*opt && mono_gc_parse_environment_string_extract_number (opt, &soft_limit)) {
					if (soft_limit <= 0) {
						sgen_env_var_error (MONO_GC_PARAMS_NAME, NULL, "`soft-heap-limit` must be positive.");
						soft_limit = 0;
					}
				} else {
					sgen_env_var_error (MONO_GC_PARAMS_NAME, NULL, "`soft-heap-limit` must be an integer.");
				}
				continue;
			}
			if (g_str_has_prefix (opt, "stack-mark=")) {
				opt = strchr (opt, '=') + 1;
				if (!strcmp (opt, "precise")) {
					conservative_stack_mark = FALSE;
				} else if (!strcmp (opt, "conservative")) {
					conservative_stack_mark = TRUE;
				} else {
					sgen_env_var_error (MONO_GC_PARAMS_NAME, conservative_stack_mark ? "Using `conservative`." : "Using `precise`.",
							"Invalid value `%s` for `stack-mark` option, possible values are: `precise`, `conservative`.", opt);
				}
				continue;
			}
			if (g_str_has_prefix (opt, "bridge-implementation=")) {
				opt = strchr (opt, '=') + 1;
				sgen_set_bridge_implementation (opt);
				continue;
			}
			if (g_str_has_prefix (opt, "toggleref-test")) {
				sgen_register_test_toggleref_callback ();
				continue;
			}

#ifdef USER_CONFIG
			if (g_str_has_prefix (opt, "nursery-size=")) {
				size_t val;
				opt = strchr (opt, '=') + 1;
				if (*opt && mono_gc_parse_environment_string_extract_number (opt, &val)) {
					if ((val & (val - 1))) {
						sgen_env_var_error (MONO_GC_PARAMS_NAME, "Using default value.", "`nursery-size` must be a power of two.");
						continue;
					}

					if (val < SGEN_MAX_NURSERY_WASTE) {
						sgen_env_var_error (MONO_GC_PARAMS_NAME, "Using default value.",
								"`nursery-size` must be at least %d bytes.", SGEN_MAX_NURSERY_WASTE);
						continue;
					}

					sgen_nursery_size = val;
					sgen_nursery_bits = 0;
					while (ONE_P << (++ sgen_nursery_bits) != sgen_nursery_size)
						;
				} else {
					sgen_env_var_error (MONO_GC_PARAMS_NAME, "Using default value.", "`nursery-size` must be an integer.");
					continue;
				}
				continue;
			}
#endif
			if (g_str_has_prefix (opt, "save-target-ratio=")) {
				double val;
				opt = strchr (opt, '=') + 1;
				if (parse_double_in_interval (MONO_GC_PARAMS_NAME, "save-target-ratio", opt,
						SGEN_MIN_SAVE_TARGET_RATIO, SGEN_MAX_SAVE_TARGET_RATIO, &val)) {
					save_target = val;
				}
				continue;
			}
			if (g_str_has_prefix (opt, "default-allowance-ratio=")) {
				double val;
				opt = strchr (opt, '=') + 1;
				if (parse_double_in_interval (MONO_GC_PARAMS_NAME, "default-allowance-ratio", opt,
						SGEN_MIN_ALLOWANCE_NURSERY_SIZE_RATIO, SGEN_MIN_ALLOWANCE_NURSERY_SIZE_RATIO, &val)) {
					allowance_ratio = val;
				}
				continue;
			}
			if (g_str_has_prefix (opt, "allow-synchronous-major=")) {
				if (!major_collector.is_concurrent) {
					sgen_env_var_error (MONO_GC_PARAMS_NAME, "Ignoring.", "`allow-synchronous-major` is only valid for the concurrent major collector.");
					continue;
				}

				opt = strchr (opt, '=') + 1;

				if (!strcmp (opt, "yes")) {
					allow_synchronous_major = TRUE;
				} else if (!strcmp (opt, "no")) {
					allow_synchronous_major = FALSE;
				} else {
					sgen_env_var_error (MONO_GC_PARAMS_NAME, "Using default value.", "`allow-synchronous-major` must be either `yes' or `no'.");
					continue;
				}
			}

			if (!strcmp (opt, "cementing")) {
				cement_enabled = TRUE;
				continue;
			}
			if (!strcmp (opt, "no-cementing")) {
				cement_enabled = FALSE;
				continue;
			}

			if (major_collector.handle_gc_param && major_collector.handle_gc_param (opt))
				continue;

			if (sgen_minor_collector.handle_gc_param && sgen_minor_collector.handle_gc_param (opt))
				continue;

			sgen_env_var_error (MONO_GC_PARAMS_NAME, "Ignoring.", "Unknown option `%s`.", opt);

			if (usage_printed)
				continue;

			fprintf (stderr, "\n%s must be a comma-delimited list of one or more of the following:\n", MONO_GC_PARAMS_NAME);
			fprintf (stderr, "  max-heap-size=N (where N is an integer, possibly with a k, m or a g suffix)\n");
			fprintf (stderr, "  soft-heap-limit=n (where N is an integer, possibly with a k, m or a g suffix)\n");
			fprintf (stderr, "  nursery-size=N (where N is an integer, possibly with a k, m or a g suffix)\n");
			fprintf (stderr, "  major=COLLECTOR (where COLLECTOR is `marksweep', `marksweep-conc', `marksweep-par')\n");
			fprintf (stderr, "  minor=COLLECTOR (where COLLECTOR is `simple' or `split')\n");
			fprintf (stderr, "  wbarrier=WBARRIER (where WBARRIER is `remset' or `cardtable')\n");
			fprintf (stderr, "  stack-mark=MARK-METHOD (where MARK-METHOD is 'precise' or 'conservative')\n");
			fprintf (stderr, "  [no-]cementing\n");
			if (major_collector.is_concurrent)
				fprintf (stderr, "  allow-synchronous-major=FLAG (where FLAG is `yes' or `no')\n");
			if (major_collector.print_gc_param_usage)
				major_collector.print_gc_param_usage ();
			if (sgen_minor_collector.print_gc_param_usage)
				sgen_minor_collector.print_gc_param_usage ();
			fprintf (stderr, " Experimental options:\n");
			fprintf (stderr, "  save-target-ratio=R (where R must be between %.2f - %.2f).\n", SGEN_MIN_SAVE_TARGET_RATIO, SGEN_MAX_SAVE_TARGET_RATIO);
			fprintf (stderr, "  default-allowance-ratio=R (where R must be between %.2f - %.2f).\n", SGEN_MIN_ALLOWANCE_NURSERY_SIZE_RATIO, SGEN_MAX_ALLOWANCE_NURSERY_SIZE_RATIO);
			fprintf (stderr, "\n");

			usage_printed = TRUE;
		}
		g_strfreev (opts);
	}

	if (major_collector_opt)
		g_free (major_collector_opt);

	if (minor_collector_opt)
		g_free (minor_collector_opt);

	alloc_nursery ();

	if (major_collector.is_concurrent && cement_enabled) {
		sgen_env_var_error (MONO_GC_PARAMS_NAME, "Ignoring.", "`cementing` is not supported on concurrent major collectors.");
		cement_enabled = FALSE;
	}

	sgen_cement_init (cement_enabled);

	if ((env = g_getenv (MONO_GC_DEBUG_NAME))) {
		gboolean usage_printed = FALSE;

		opts = g_strsplit (env, ",", -1);
		for (ptr = opts; ptr && *ptr; ptr ++) {
			char *opt = *ptr;
			if (!strcmp (opt, ""))
				continue;
			if (opt [0] >= '0' && opt [0] <= '9') {
				gc_debug_level = atoi (opt);
				opt++;
				if (opt [0] == ':')
					opt++;
				if (opt [0]) {
					char *rf = g_strdup_printf ("%s.%d", opt, mono_process_current_pid ());
					gc_debug_file = fopen (rf, "wb");
					if (!gc_debug_file)
						gc_debug_file = stderr;
					g_free (rf);
				}
			} else if (!strcmp (opt, "print-allowance")) {
				debug_print_allowance = TRUE;
			} else if (!strcmp (opt, "print-pinning")) {
				sgen_pin_stats_enable ();
			} else if (!strcmp (opt, "verify-before-allocs")) {
				verify_before_allocs = 1;
				has_per_allocation_action = TRUE;
			} else if (g_str_has_prefix (opt, "verify-before-allocs=")) {
				char *arg = strchr (opt, '=') + 1;
				verify_before_allocs = atoi (arg);
				has_per_allocation_action = TRUE;
			} else if (!strcmp (opt, "collect-before-allocs")) {
				collect_before_allocs = 1;
				has_per_allocation_action = TRUE;
			} else if (g_str_has_prefix (opt, "collect-before-allocs=")) {
				char *arg = strchr (opt, '=') + 1;
				has_per_allocation_action = TRUE;
				collect_before_allocs = atoi (arg);
			} else if (!strcmp (opt, "verify-before-collections")) {
				whole_heap_check_before_collection = TRUE;
			} else if (!strcmp (opt, "check-at-minor-collections")) {
				consistency_check_at_minor_collection = TRUE;
				nursery_clear_policy = CLEAR_AT_GC;
			} else if (!strcmp (opt, "mod-union-consistency-check")) {
				if (!major_collector.is_concurrent) {
					sgen_env_var_error (MONO_GC_DEBUG_NAME, "Ignoring.", "`mod-union-consistency-check` only works with concurrent major collector.");
					continue;
				}
				mod_union_consistency_check = TRUE;
			} else if (!strcmp (opt, "check-mark-bits")) {
				check_mark_bits_after_major_collection = TRUE;
			} else if (!strcmp (opt, "check-nursery-pinned")) {
				check_nursery_objects_pinned = TRUE;
			} else if (!strcmp (opt, "clear-at-gc")) {
				nursery_clear_policy = CLEAR_AT_GC;
			} else if (!strcmp (opt, "clear-nursery-at-gc")) {
				nursery_clear_policy = CLEAR_AT_GC;
			} else if (!strcmp (opt, "clear-at-tlab-creation")) {
				nursery_clear_policy = CLEAR_AT_TLAB_CREATION;
			} else if (!strcmp (opt, "debug-clear-at-tlab-creation")) {
				nursery_clear_policy = CLEAR_AT_TLAB_CREATION_DEBUG;
			} else if (!strcmp (opt, "check-scan-starts")) {
				do_scan_starts_check = TRUE;
			} else if (!strcmp (opt, "verify-nursery-at-minor-gc")) {
				do_verify_nursery = TRUE;
			} else if (!strcmp (opt, "check-concurrent")) {
				if (!major_collector.is_concurrent) {
					sgen_env_var_error (MONO_GC_DEBUG_NAME, "Ignoring.", "`check-concurrent` only works with concurrent major collectors.");
					continue;
				}
				do_concurrent_checks = TRUE;
			} else if (!strcmp (opt, "dump-nursery-at-minor-gc")) {
				do_dump_nursery_content = TRUE;
			} else if (!strcmp (opt, "no-managed-allocator")) {
				sgen_set_use_managed_allocator (FALSE);
			} else if (!strcmp (opt, "disable-minor")) {
				disable_minor_collections = TRUE;
			} else if (!strcmp (opt, "disable-major")) {
				disable_major_collections = TRUE;
			} else if (g_str_has_prefix (opt, "heap-dump=")) {
				char *filename = strchr (opt, '=') + 1;
				nursery_clear_policy = CLEAR_AT_GC;
				sgen_debug_enable_heap_dump (filename);
			} else if (g_str_has_prefix (opt, "binary-protocol=")) {
				char *filename = strchr (opt, '=') + 1;
				char *colon = strrchr (filename, ':');
				size_t limit = -1;
				if (colon) {
					if (!mono_gc_parse_environment_string_extract_number (colon + 1, &limit)) {
						sgen_env_var_error (MONO_GC_DEBUG_NAME, "Ignoring limit.", "Binary protocol file size limit must be an integer.");
						limit = -1;
					}
					*colon = '\0';
				}
				binary_protocol_init (filename, (long long)limit);
			} else if (!strcmp (opt, "nursery-canaries")) {
				do_verify_nursery = TRUE;
				sgen_set_use_managed_allocator (FALSE);
				enable_nursery_canaries = TRUE;
			} else if (!strcmp (opt, "do-not-finalize")) {
				do_not_finalize = TRUE;
			} else if (!strcmp (opt, "log-finalizers")) {
				log_finalizers = TRUE;
			} else if (!sgen_client_handle_gc_debug (opt) && !sgen_bridge_handle_gc_debug (opt)) {
				sgen_env_var_error (MONO_GC_DEBUG_NAME, "Ignoring.", "Unknown option `%s`.", opt);

				if (usage_printed)
					continue;

				fprintf (stderr, "\n%s must be of the format [<l>[:<filename>]|<option>]+ where <l> is a debug level 0-9.\n", MONO_GC_DEBUG_NAME);
				fprintf (stderr, "Valid <option>s are:\n");
				fprintf (stderr, "  collect-before-allocs[=<n>]\n");
				fprintf (stderr, "  verify-before-allocs[=<n>]\n");
				fprintf (stderr, "  check-at-minor-collections\n");
				fprintf (stderr, "  check-mark-bits\n");
				fprintf (stderr, "  check-nursery-pinned\n");
				fprintf (stderr, "  verify-before-collections\n");
				fprintf (stderr, "  verify-nursery-at-minor-gc\n");
				fprintf (stderr, "  dump-nursery-at-minor-gc\n");
				fprintf (stderr, "  disable-minor\n");
				fprintf (stderr, "  disable-major\n");
				fprintf (stderr, "  check-concurrent\n");
				fprintf (stderr, "  clear-[nursery-]at-gc\n");
				fprintf (stderr, "  clear-at-tlab-creation\n");
				fprintf (stderr, "  debug-clear-at-tlab-creation\n");
				fprintf (stderr, "  check-scan-starts\n");
				fprintf (stderr, "  no-managed-allocator\n");
				fprintf (stderr, "  print-allowance\n");
				fprintf (stderr, "  print-pinning\n");
				fprintf (stderr, "  heap-dump=<filename>\n");
				fprintf (stderr, "  binary-protocol=<filename>[:<file-size-limit>]\n");
				fprintf (stderr, "  nursery-canaries\n");
				fprintf (stderr, "  do-not-finalize\n");
				fprintf (stderr, "  log-finalizers\n");
				sgen_client_print_gc_debug_usage ();
				sgen_bridge_print_gc_debug_usage ();
				fprintf (stderr, "\n");

				usage_printed = TRUE;
			}
		}
		g_strfreev (opts);
	}

	if (check_mark_bits_after_major_collection)
		nursery_clear_policy = CLEAR_AT_GC;

	if (major_collector.post_param_init)
		major_collector.post_param_init (&major_collector);

	if (major_collector.needs_thread_pool)
		sgen_workers_init (1);

	sgen_memgov_init (max_heap, soft_limit, debug_print_allowance, allowance_ratio, save_target);

	memset (&remset, 0, sizeof (remset));

	sgen_card_table_init (&remset);

	gc_initialized = 1;
}

NurseryClearPolicy
sgen_get_nursery_clear_policy (void)
{
	return nursery_clear_policy;
}

void
sgen_gc_lock (void)
{
	LOCK_GC;
}

void
sgen_gc_unlock (void)
{
	gboolean try_free = sgen_try_free_some_memory;
	sgen_try_free_some_memory = FALSE;
	mono_mutex_unlock (&gc_mutex);
	if (try_free)
		mono_thread_hazardous_try_free_some ();
}

void
sgen_major_collector_iterate_live_block_ranges (sgen_cardtable_block_callback callback)
{
	major_collector.iterate_live_block_ranges (callback);
}

SgenMajorCollector*
sgen_get_major_collector (void)
{
	return &major_collector;
}

void mono_gc_set_skip_thread (gboolean skip)
{
	SgenThreadInfo *info = mono_thread_info_current ();

	LOCK_GC;
	info->gc_disabled = skip;
	UNLOCK_GC;
}

SgenRememberedSet*
sgen_get_remset (void)
{
	return &remset;
}

void
mono_gc_register_altstack (gpointer stack, gint32 stack_size, gpointer altstack, gint32 altstack_size)
{
	// FIXME:
}

static void
count_cards (long long *major_total, long long *major_marked, long long *los_total, long long *los_marked)
{
	sgen_get_major_collector ()->count_cards (major_total, major_marked);
	sgen_los_count_cards (los_total, los_marked);
}

static gboolean world_is_stopped = FALSE;

/* LOCKING: assumes the GC lock is held */
int
sgen_stop_world (int generation)
{
	int count;
	long long major_total = -1, major_marked = -1, los_total = -1, los_marked = -1;

	SGEN_ASSERT (0, !world_is_stopped, "Why are we stopping a stopped world?");

	binary_protocol_world_stopping (generation, sgen_timestamp ());

	count = sgen_client_stop_world (generation);

	world_is_stopped = TRUE;

	if (binary_protocol_is_heavy_enabled ())
		count_cards (&major_total, &major_marked, &los_total, &los_marked);
	binary_protocol_world_stopped (generation, sgen_timestamp (), major_total, major_marked, los_total, los_marked);

	return count;
}

/* LOCKING: assumes the GC lock is held */
int
sgen_restart_world (int generation, GGTimingInfo *timing)
{
	int count;

	SGEN_ASSERT (0, world_is_stopped, "Why are we restarting a running world?");

	if (binary_protocol_is_enabled ()) {
		long long major_total = -1, major_marked = -1, los_total = -1, los_marked = -1;
		if (binary_protocol_is_heavy_enabled ())
			count_cards (&major_total, &major_marked, &los_total, &los_marked);
		binary_protocol_world_restarting (generation, sgen_timestamp (), major_total, major_marked, los_total, los_marked);
	}

	count = sgen_client_restart_world (generation, timing);

	world_is_stopped = FALSE;

	binary_protocol_world_restarted (generation, sgen_timestamp ());

	sgen_try_free_some_memory = TRUE;

	if (sgen_need_bridge_processing ())
		sgen_bridge_processing_finish (generation);

	sgen_memgov_collection_end (generation, timing, timing ? 2 : 0);

	return count;
}

gboolean
sgen_is_world_stopped (void)
{
	return world_is_stopped;
}

void
sgen_check_whole_heap_stw (void)
{
	sgen_stop_world (0);
	sgen_clear_nursery_fragments ();
	sgen_check_whole_heap (FALSE);
	sgen_restart_world (0, NULL);
}

gint64
sgen_timestamp (void)
{
	SGEN_TV_DECLARE (timestamp);
	SGEN_TV_GETTIME (timestamp);
	return SGEN_TV_ELAPSED (sgen_init_timestamp, timestamp);
}

#endif /* HAVE_SGEN_GC */
