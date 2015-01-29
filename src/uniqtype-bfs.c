#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <search.h>
#include <assert.h>
#include <string.h>
#include <err.h>
#include "uniqtype.h"
#include "uniqtype-bfs.h"

/* debugging */
FILE* debug_out = NULL;
#ifndef DEBUGGING_OUTPUT_FILENAME
#define DEBUGGING_OUTPUT_FILENAME NULL
#endif
const char *debugging_output_filename = DEBUGGING_OUTPUT_FILENAME;
#define DEBUG_GUARD(stmt) do { if (debug_out != NULL) { stmt; } } while(0)

static void build_adjacency_list_recursive(
	node_rec **p_adj_u_head, node_rec **p_adj_u_tail, 
	void *obj_start, struct uniqtype *obj_t, 
	unsigned long start_offset, struct uniqtype *t_at_offset, 
	follow_ptr_fn *follow_ptr, void *fp_arg);

enum node_colour { WHITE, GREY, BLACK }; // WHITE == 0, so "absent" pair->v 0 means WHITE

static node_rec *make_node(void *obj, struct uniqtype *t);

static _Bool queue_empty(void *q_head)
{
	return !q_head;
}

static void queue_push_tail(node_rec **q_head, node_rec **q_tail, node_rec *to_enqueue)
{
	node_rec *old_head_node = *q_head;
	node_rec *old_tail_node = *q_tail;
	assert(!to_enqueue->next);
	*q_tail = to_enqueue;
	if (old_tail_node) old_tail_node->next = to_enqueue;
	else
	{
		assert(!old_head_node);
		/* If we just went from 0 elements to 1, update the head */
		*q_head = to_enqueue;
	}
}

static node_rec *queue_pop_head(node_rec **q_head, node_rec **q_tail)
{
	node_rec *old_head_node = *q_head;
	if (old_head_node)
	{
		*q_head = old_head_node->next;
		/* If we just went from 1 element to 0, clear the tail. */
		if (!*q_head) *q_tail = NULL;
		/* Clear the "next" pointer, since it's not in the queue any more. */
		old_head_node->next = NULL;
	}
	return old_head_node;
}

struct pair
{
	const void *k;
	uintptr_t v;
};
/* The tsearch man page says 
 * "The first field in each node of the tree is a pointer to the corresponding
 * data item." */
typedef struct internal_treenode_s
{
	void *key;
} internal_treenode;
#define TREENODE_TO_PAIR_PTR(n) ((struct pair *)((n)->key))

static int compar(const void *key1, const void *key2)
{
	return (uintptr_t) ((struct pair *) key1)->k - (uintptr_t) ((struct pair *) key2)->k;
}

static uintptr_t treemap_get(void *const *rootp, const void *key)
{
	/* Find an existing pair, if there is one. */
	struct pair p = { key, 0 };
	internal_treenode *found = tfind(&p, rootp, compar);
	if (found)
	{
		assert(TREENODE_TO_PAIR_PTR(found)->v != 0);
		return TREENODE_TO_PAIR_PTR(found)->v; 
	}
	return 0ul;
}

static void treemap_set(void **rootp, const void *key, uintptr_t value)
{
	/* Find an existing pair, if there is one. */
	struct pair p = { key, value };
	internal_treenode *found = tfind(&p, rootp, compar);
	if (found)
	{
		TREENODE_TO_PAIR_PTR(found)->v = value;
	}
	else
	{
		/* malloc a new node */
		struct pair *p_new_pair = malloc(sizeof (struct pair));
		if (!p_new_pair) { warn("insufficient memory"); abort(); }
		/* initialize the new node with p's k/v */
		memcpy(p_new_pair, &p, sizeof (struct pair));
		/* tsearch will add it */
		internal_treenode *inserted = tsearch(p_new_pair, rootp, compar);
		assert(inserted->key == p_new_pair);
		assert(treemap_get(rootp, key) == value);
	}
}

static void treemap_delete(void **rootp)
{
	tdestroy(*rootp, free);
}

#define NAME_FOR_UNIQTYPE(u) ((u) ? ((u)->name ? (u)->name : "(no name)") : "(no type)")
/* HACK: archdep */
#define IS_PLAUSIBLE_POINTER(p) (!(p) || ((p) == (void*) -1) || (((uintptr_t) (p)) >= 4194304 && ((uintptr_t) (p)) < 0x800000000000ul))

/* This function builds an adjacency list for the current node, by adding
 * *all* nodes, not just (despite the name) those pointed to by subobjects.
 * i.e. the top-level object is a zero-degree subobject. */
static void build_adjacency_list_recursive(
	node_rec **p_adj_u_head, node_rec **p_adj_u_tail, 
	void *obj_start, struct uniqtype *obj_t, 
	unsigned long start_offset, struct uniqtype *t_at_offset, 
	follow_ptr_fn *follow_ptr, void *fp_arg)
{
	if (t_at_offset == &__uniqtype__void) return;
	
	fprintf(stderr, "Descending through subobjects of object at %p, "
		"currently at subobject offset %ld of type %s\n",
		obj_start, start_offset, NAME_FOR_UNIQTYPE(t_at_offset));
	
	// If someone tries to walk_bfs from a function pointer, we will try to
	// bootstrap the list from a queue consisting of a single object (the function)
	// and no type. If so, the list is already complete (i.e. empty), so return
	if (!t_at_offset) return;
	if (!UNIQTYPE_HAS_SUBOBJECTS(t_at_offset)) return;

	/* The way we iterate through structs and arrays is different. */
	struct contained *contained = &t_at_offset->contained[0];
	unsigned nmemb; 
	_Bool is_array;

	if (UNIQTYPE_HAS_DATA_MEMBERS(t_at_offset))
	{
		is_array = 0;
		nmemb = t_at_offset->nmemb;
		/* FIXME: toplevel of heap arrays */
	}
	else
	{
		is_array = 1;
		nmemb = t_at_offset->array_len; /* FIXME: dynamically-sized arrays */
	}

	for (unsigned i = 0; i < nmemb; ++i, contained += (is_array ? 0 : 1))
	{
		// if we're an array, the element type should have known length (pos_maxoff)
		assert(!is_array || UNIQTYPE_HAS_KNOWN_LENGTH(contained->ptr));
		long memb_offset = is_array ? (i * contained->ptr->pos_maxoff) : contained->offset;
		
		/* Is it a pointer? If so, add it to the adjacency list. */
		if (UNIQTYPE_IS_POINTER_TYPE(contained->ptr))
		{
			struct uniqtype *pointed_to_static_t = UNIQTYPE_POINTEE_TYPE(contained->ptr);
			// get the address of the pointed-to object
			void *pointed_to_object = *(void**)((char*) obj_start + start_offset + memb_offset);
			/* Check sanity of the pointer. We might be reading some union'd storage
			 * that is currently holding a non-pointer. */
			node_rec *to_enqueue = NULL;
			if (pointed_to_object && IS_PLAUSIBLE_POINTER(pointed_to_object))
			{
				/* make a node and put it in the adjacency list */
				void *ptr = pointed_to_object;
				struct uniqtype *t = pointed_to_static_t;
				follow_ptr(&ptr, &t, fp_arg);
				if (ptr)
				{
					to_enqueue = make_node(ptr, t);
					queue_push_tail(p_adj_u_head, p_adj_u_tail, to_enqueue);

					DEBUG_GUARD(fprintf(debug_out, "\t%s_at_%p -> %s_at_%p;\n", 
						NAME_FOR_UNIQTYPE(obj_t), obj_start,
						NAME_FOR_UNIQTYPE(to_enqueue->t), to_enqueue->obj));
				}
			}
			else if (!pointed_to_object || pointed_to_object == (void*) -1)
			{
				/* null pointer */
			}
			else
			{
				fprintf(stderr, "Warning: insane pointer value %p found in field index %d in object %p, type %s\n",
					pointed_to_object,
					i,
					(char*) obj_start + start_offset,
					NAME_FOR_UNIQTYPE(t_at_offset)
				);
			}
			if (to_enqueue && to_enqueue->obj) fprintf(stderr, "Found a pointed-to object at %p, statically of type %s, "
				"added as %p of type %s\n",
				pointed_to_object, NAME_FOR_UNIQTYPE(pointed_to_static_t),
				to_enqueue->obj, NAME_FOR_UNIQTYPE(to_enqueue->t));
		}
		else if (UNIQTYPE_HAS_DATA_MEMBERS(contained->ptr)) /* Else is it a thing with structure? If so, recurse. */
		{
			build_adjacency_list_recursive(
				p_adj_u_head, p_adj_u_tail, 
				obj_start, obj_t, 
				start_offset + memb_offset, contained->ptr,
				follow_ptr, fp_arg
			);
		}
	}
}

static void process_bfs_queue_and_maps(
	node_rec **p_q_head,
	node_rec **p_q_tail,
	void **p_colours_root,
	void **p_distances_root,
	void **p_predecessors_root,
	follow_ptr_fn *follow_ptr, void *fp_arg,
	on_blacken_fn *on_blacken, void *ob_arg)
{
	while (!queue_empty(*p_q_head))
	{
		node_rec *u = queue_pop_head(p_q_head, p_q_tail);
	
		treemap_set(p_colours_root, u->obj, GREY);
		
		/* create the adjacency list for u, by flattening the subobject hierarchy */
		node_rec *adj_u_head = NULL;
		node_rec *adj_u_tail = NULL;
		build_adjacency_list_recursive(&adj_u_head, &adj_u_tail, 
			u->obj, u->t, 
			/* start offset */ 0, u->t, 
			follow_ptr, fp_arg
		);
		/* ^-- this starts at the top-level subobject, i.e. the object, so it builds
		 * the complete adjacency list for this node. */

		/* now that we have the adjacency list, enqueue any adjacent nodes that are white */
		node_rec *v;
		while ((v = queue_pop_head(&adj_u_head, &adj_u_tail)) != NULL)
		{
			/* We initialise all nodes' colours to NULL a.k.a. WHITE */
			uintptr_t colour = treemap_get(p_colours_root, v->obj);
			fprintf(stderr, "From object at %p, type %s, considering adjacent object at %p, of type %s, colour %s\n",
				u->obj, NAME_FOR_UNIQTYPE(u->t),
				v->obj, NAME_FOR_UNIQTYPE(v->t), 
				(colour == WHITE) ? "white" : (colour == GREY) ? "grey" : (colour == BLACK) ? "black" : "unknown"
			);
			if (colour == WHITE)
			{
				treemap_set(p_colours_root, v->obj, GREY);
				treemap_set(p_distances_root, v->obj, treemap_get(p_distances_root, v->obj) + 1);
				treemap_set(p_predecessors_root, v->obj, (uintptr_t) v->obj);
				fprintf(stderr, "Enqueued object at %p, type %s\n", 
					v->obj, NAME_FOR_UNIQTYPE(v->t));
				queue_push_tail(p_q_head, p_q_tail, v); // the queue takes our copy of v, which we're finished with
			}
			else free(v);
		}

		/* blacken u, and call the function for it */
		treemap_set(p_colours_root, u->obj, BLACK);
		on_blacken(u->obj, u->t, ob_arg);
		free(u);
		
		/* Note that it doesn't matter if u's address gets recycled, because 
		 * we don't use it as a key in a map -- object addresses are keys. */
	}
	DEBUG_GUARD(fflush(debug_out));
	DEBUG_GUARD(fprintf(debug_out, "}\n"));
}
void __uniqtype_process_bfs_queue(
	node_rec **p_q_head,
	node_rec **p_q_tail,
	follow_ptr_fn *follow_ptr, void *fp_arg,
	on_blacken_fn *on_blacken, void *ob_arg)
{
	void *colours_root = NULL; /* map void* -> node_colour */
	void *distances_root = NULL; /* map void* -> int */
	void *predecessors_root = NULL; /* map void* -> void* */
	
	process_bfs_queue_and_maps(p_q_head, p_q_tail, 
		&colours_root, &distances_root, &predecessors_root,
		follow_ptr, fp_arg,
		on_blacken, ob_arg
	);
	treemap_delete(&colours_root);
	treemap_delete(&distances_root);
	treemap_delete(&predecessors_root);
}
void __uniqtype_walk_bfs_from_object(
	void *object, struct uniqtype *t,
	follow_ptr_fn *follow_ptr, void *fp_arg,
	on_blacken_fn *on_blacken, void *ob_arg)
{
	if (!object) return;
	/* We are doing breadth-first search through the object graph rooted at object,
	 * using object_ref_derefed_offsets[object_rep][object_form] to make an adjacency list
	 * out of the actual object graph.*/
	 
	/* init debug output */
	if (debug_out == NULL && debugging_output_filename != NULL)
	{
		debug_out = fopen(debugging_output_filename, "r+");
	}
	DEBUG_GUARD(fprintf(debug_out, "digraph view_from_%p {\n", object));
	
	node_rec *q_head = NULL;
	node_rec *q_tail = NULL;
	
	/* Make an initial node. Don't adjust the pointer. */
	node_rec *to_enqueue = make_node(object, t);
	queue_push_tail(&q_head, &q_tail, to_enqueue);
	
	/* Sanity check: assert that our object's start is non-null and within 128MB of our pointer. */
	assert(!to_enqueue || ((char*) to_enqueue->obj <= (char*) object
						&& (char*) object - (char*)to_enqueue->obj < (1U<<27)));
	
	__uniqtype_process_bfs_queue(&q_head, &q_tail, follow_ptr, fp_arg, on_blacken, ob_arg);
	
	DEBUG_GUARD(fprintf(debug_out, "}\n"));
}

static node_rec *make_node(void *obj, struct uniqtype *t)
{
	node_rec *node = calloc(1, sizeof (node_rec));
	if (!node) { warn("insufficient memory"); abort(); }
	node->obj = obj;
	node->t = t;
	return node;
}

void __uniqtype_default_follow_ptr(void **p_obj, struct uniqtype **p_t, void *arg)
{ /* no-op */ }