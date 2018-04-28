#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "mm.h"
#include "memlib.h"
/* for the free list ranges */
#include <limits.h>

/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8
/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~0x7)
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

/* block metadata macros - b_ptr is a block pointer, p_ptr is a payload pointer */
#define BLOCK_SIZE(b_ptr) (*(size_t *)(b_ptr) & ~1L)
#define PREV_BLOCK(b_ptr) ((char *)(b_ptr) - BLOCK_SIZE(((char *)(b_ptr) - SIZE_T_SIZE)))
#define NEXT_BLOCK(b_ptr) (((void *)b_ptr) + BLOCK_SIZE(b_ptr))
#define BLOCK_HEADER(p_ptr) (((void *)(p_ptr)) - SIZE_T_SIZE)
#define BLOCK_FOOTER(b_ptr) (NEXT_BLOCK(b_ptr) - SIZE_T_SIZE)
#define ALLOCATED(b_ptr) ((*(size_t *)(b_ptr)) & 1)

/* segregated fit structures and manipulation functions */
#define SENTINEL_SIZE 0
#define NUM_SIZE_CLASSES 5
#define MIN_BLK_SIZE 32
/* pads a size with PADDING bytes - useful for realloc. if size is less than min block size, will substitute the min size */
#define PADDING 128
#define PAD(size) ((((size) < MIN_BLK_SIZE) ? MIN_BLK_SIZE : (size)) + PADDING)
size_t min_class_size[] = {MIN_BLK_SIZE, 256, 512, 1024, 2048, INT_MAX};

typedef struct free_blk_header {
  size_t size;
  struct free_blk_header *next;
  struct free_blk_header *prior;
} free_blk_header_t;

free_blk_header_t *free_lists;

static void* prologue;
static void* epilogue;

/* 'macro' functions - not used in production, but useful for debugging */
size_t block_size(void *block){
  return *(size_t *)(block) & ~1L;
}
void *prev_block(void *block){
  size_t prev_size = *(size_t *)(block - SIZE_T_SIZE) & ~0x7;
  return block - prev_size;
}
void *next_block(void *block){
  return block + block_size(block);
}
/* this can be used to convert payload to regular block pointers */
void *block_header(void *payload){
  return ((char *)payload) - SIZE_T_SIZE;
}
void *block_footer(void *block){
  return next_block(block) - SIZE_T_SIZE;
}
int allocated(void* block){
  return *(size_t *)(block) & 1;
} 

void *populate_alloc_blk_tags(void *block, size_t size){
  size_t *header = (size_t *)block;
  size_t *footer = BLOCK_FOOTER(block);
  *header =  size | 1;
  *footer = size | 1;
  return block;
}

free_blk_header_t *populate_free_blk_tags(void *block, size_t size){
  free_blk_header_t *header = (free_blk_header_t *)(block);
  size_t *footer = block + size - SIZE_T_SIZE;
  *footer = size;
  header->size = size;
  header->next = NULL;
  header->prior = NULL;
  return header;
}

/* keep in mind the head is a sentinel head that is circularly linked */
free_blk_header_t *ll_free_blk_prepend(free_blk_header_t *head, free_blk_header_t *new){
  new->prior = head;
  new->next = head->next;
  head->next = head->next->prior = new;
  return head;
}

void ll_free_blk_remove(free_blk_header_t *node){
  node->prior->next = node->next;
  node->next->prior = node->prior;
}

free_blk_header_t *ll_free_blk_search(free_blk_header_t *head, size_t size){
  free_blk_header_t *node = head->next;
  while (node != head){
    if (node->size >= size){
      return node;
    }
    node = node->next;
  }
  return NULL;
}

void free_list_insert(free_blk_header_t *block){
  size_t size = block->size;
  free_blk_header_t *list = NULL;
  int i;
  /*find the correct list, traversing backwards */
  for (i = NUM_SIZE_CLASSES - 1; i >= 0 ; i--){
    if (min_class_size[i] <= size ){
      list = &free_lists[i];
      break;
    }
  }
  ll_free_blk_prepend(list, block);
}

/* split will be called during malloc - leftover block space from a malloc request should be redistributed into the appropriate free list. the original free block minus leftovers will be returned to the caller */
free_blk_header_t *split_and_replace_free_blk(free_blk_header_t *block, size_t request_size){
  size_t placement_size = block->size - request_size;
  /* can't split, just return the original block */
  if (placement_size < MIN_BLK_SIZE){
    return block;
  }
  /* split the block */
  block->size = request_size;
  
  /* place the leftover space */
  free_blk_header_t *leftover = populate_free_blk_tags((char *)block + request_size, placement_size);
  free_list_insert(leftover);
  return block;
}

/* returns in order:
   - a node in a segfit list that would work
   - NULL, no fit found - we should sbrk in malloc
*/
free_blk_header_t *good_fit(size_t size){
  int i;
  free_blk_header_t *node = NULL;
  /* traverse lists forwards, finding first nonempty list*/
  for (i = 0; i < NUM_SIZE_CLASSES; i++){
    if (min_class_size[i] >= size && free_lists[i].next != &free_lists[i]){
      node = free_lists[i].next;
    }
  }
  /* no list found, search the catchall list */
  if (node == NULL){
    node = ll_free_blk_search(&free_lists[NUM_SIZE_CLASSES - 1], size);
  }
  /* no suitable fit exists - returns NULL, signaling to malloc we should sbrk */
  return node;
}

free_blk_header_t *coalesce(free_blk_header_t *just_freed){
  /* the sentinel size in the sentinel free block removes having to check for it */
  int next_allocated = ALLOCATED(NEXT_BLOCK((void *)just_freed));
  int prev_allocated = ALLOCATED(PREV_BLOCK((void *)just_freed));
  free_blk_header_t *new = NULL;
  size_t new_size;
  if (next_allocated && prev_allocated){
    /* case 1, from slides - do nothing! */
    return just_freed;
  }
  else if (next_allocated && !prev_allocated){
    /* case 2 */
    ll_free_blk_remove((free_blk_header_t *)PREV_BLOCK(just_freed));
    new_size = just_freed->size + BLOCK_SIZE(PREV_BLOCK(just_freed));
    new = populate_free_blk_tags(PREV_BLOCK(just_freed), new_size);
  }
  else if (!next_allocated && prev_allocated){
    /* case 3 */
    ll_free_blk_remove((free_blk_header_t *)NEXT_BLOCK(just_freed));
    new_size = just_freed->size + BLOCK_SIZE(NEXT_BLOCK(just_freed));
    new = populate_free_blk_tags(just_freed, new_size);
  }
  else if (!next_allocated && !prev_allocated){
    /* case 4 */
    ll_free_blk_remove((free_blk_header_t *)NEXT_BLOCK(just_freed));
    ll_free_blk_remove((free_blk_header_t *)PREV_BLOCK(just_freed));
    new_size = just_freed->size + BLOCK_SIZE(PREV_BLOCK(just_freed)) + BLOCK_SIZE(NEXT_BLOCK(just_freed));
    new = populate_free_blk_tags(PREV_BLOCK(just_freed), new_size);
  }
  return new;
}

/* wraps sbrk, mainting a sentinel footer at the top of the heap */
free_blk_header_t *grow_heap(size_t size){
  /* adjust pointer - this will essentially overwrite the previous epilogue and make space for the new one */
  void *new_area = mem_sbrk(size) - SIZE_T_SIZE;
  free_blk_header_t *new_block = populate_free_blk_tags(new_area, size);
  /* adjust epilogue */
  epilogue = NEXT_BLOCK(new_block);
  *(size_t *)epilogue = SIZE_T_SIZE | 1;
  return new_block;
}

/* debug functions */
void dbg_p_heap(void){
  int alloc = 0;
  size_t size = 0;
  /* skip the prologue and free lists */
  void *block = prologue + 2*SIZE_T_SIZE;
  printf("-------------------------------------\n");
  printf("addr\t\thdr\talloc\tftr\t\n");
  printf("-------------------------------------\n");
  while (block != epilogue){
    size = block_size(block);
    alloc = allocated(block);
    printf("%p\t%zu\t%s\t%zu\n", block, size, (alloc) ? "true" : "false", block_size(block_footer(block)));
    block = next_block(block);
  }
}

void dbg_p_free_list(free_blk_header_t *head){
  free_blk_header_t *node = head->next;
  while (node != head){
    printf("[%zu] ", block_size((void*)node));
    node = node->next;
  }
  printf("\n");
}

void dbg_p_all_lists(void){
  int i;
  char high_range[10];
  for (i = 0; i < NUM_SIZE_CLASSES; i ++){
    snprintf(high_range, sizeof(high_range), "%zu", min_class_size[i+1]);
    printf("list %d (%zu-%s):\t", i, min_class_size[i], (min_class_size[i+1] == INT_MAX) ? "INT MAX" : high_range);
    dbg_p_free_list(&free_lists[i]);
  }
}

int mm_init(void){
  int i;
  size_t prologue_size = 2*SIZE_T_SIZE;
  size_t epilogue_hdr_size = SIZE_T_SIZE;
  size_t free_lists_size = NUM_SIZE_CLASSES * sizeof(free_blk_header_t);
  /* allocate sentinel free block headers */
  free_lists = mem_sbrk(free_lists_size + prologue_size + epilogue_hdr_size);
  for (i = 0; i < NUM_SIZE_CLASSES; i++){
    free_lists[i].size = SENTINEL_SIZE;
    free_lists[i].next = free_lists[i].prior = &free_lists[i];
  }
  /* set prologue/epilogue */
  prologue = mem_heap_lo() + free_lists_size;
  epilogue = mem_heap_lo() + free_lists_size + prologue_size;
  /* set prologue/epilogue header and footer */
  *(size_t *)prologue = prologue_size | 1;
  *(size_t *)((char *)prologue + SIZE_T_SIZE) = prologue_size | 1;
  *(size_t *)epilogue = epilogue_hdr_size | 1;
  return 0;
}

void *mm_malloc(size_t size){
  size_t search_size = ALIGN(PAD(size) + 2*SIZE_T_SIZE);
  free_blk_header_t *fit = good_fit(search_size);
  /* if a fit was found, try splitting and then remove the fit from its list - otherwise, grow heap*/
  if (fit){
    fit = split_and_replace_free_blk(fit, search_size);
    ll_free_blk_remove(fit);
  }
  else {
    fit = grow_heap(search_size);
  }
  populate_alloc_blk_tags((void *)fit, fit->size);
  return (char *)fit + SIZE_T_SIZE;
}

void mm_free(void *ptr){
  free_blk_header_t *freed = populate_free_blk_tags(BLOCK_HEADER(ptr), BLOCK_SIZE(BLOCK_HEADER(ptr)));
  free_blk_header_t *coalesced = coalesce(freed);
  free_list_insert(coalesced);
}

void *mm_realloc(void *ptr, size_t size){
  size_t old_size = BLOCK_SIZE(BLOCK_HEADER(ptr));
  size_t new_size = ALIGN(size + 2*SIZE_T_SIZE);
  /* trivial cases */
  if (ptr == NULL){
    return mm_malloc(size);
  }
  else if (size == 0){
    mm_free(ptr);
    return NULL;
  }
  else if (old_size >= new_size){
    return ptr;
  }
  void *next_block = NEXT_BLOCK(BLOCK_HEADER(ptr));
  size_t next_size = BLOCK_SIZE(NEXT_BLOCK(BLOCK_HEADER(ptr)));
  size_t *header, *footer;
  /* next block free and big enough - just grow into it */
  if (new_size <= next_size + old_size && !ALLOCATED(next_block)){
    ll_free_blk_remove(next_block);
    /* tag manipulation needs to be done manually here because we're jumping all over the place */
    header = BLOCK_HEADER(ptr);
    footer = BLOCK_FOOTER(next_block);
    *header = (next_size + old_size) | 1;
    *footer = (next_size + old_size) | 1;
    return ptr;
  }
  /* next block free, too small, but is the last block - grow the heap by just enough to fit the new size */
  else if (!ALLOCATED(next_block) && NEXT_BLOCK(next_block) == epilogue){
    ll_free_blk_remove(next_block);
    grow_heap(new_size - old_size + next_size);
    header = BLOCK_HEADER(ptr);
    footer = BLOCK_FOOTER(NEXT_BLOCK(next_block));
    *header = new_size | 1;
    *footer = new_size | 1;
    return ptr;
  }
  /* the original block is the last one - grow heap to fit it */
  else if (next_block == epilogue){
    grow_heap(new_size - old_size);
    header = BLOCK_HEADER(ptr);
    footer = epilogue - SIZE_T_SIZE;
    *header = new_size | 1;
    *footer = new_size | 1;
    return ptr;
  }
  /* need to actually reallocate and copy */
  else {
    void *new = mm_malloc(size);
    memcpy(new, ptr, old_size - 2*SIZE_T_SIZE);
    mm_free(ptr);
    return new;
  }
}
