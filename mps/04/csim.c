#include "cachelab.h"
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#define INT_SIZE 64
typedef enum {FALSE, TRUE} bool; 

/* cache access states are a combination of misses/hits/evictions - 2-tuple (combination of hit, miss, miss/eviction). evictions only follow misses, so we can simplify states */
/* ex. C_M_ME means the combination of a cache miss, then a miss eviction */
typedef enum {C_MISS = 2, C_HIT = 3, C_MISS_EVICTION = 5, C_M_M = 4 , C_M_H = 8, C_M_ME = 32, C_H_M = 81, C_H_H = 27, C_H_ME = 243, C_ME_M = 25, C_ME_H = 125, C_ME_ME = 3125} cache_access_state_t;

/* the permutations of cache access type are encoded using Godel numbering - see note */
cache_access_state_t combine(cache_access_state_t a, cache_access_state_t b){
  return (int) pow(a,b);
}

const char *state_name(cache_access_state_t s){
  const char *string;
  switch (s){
  case C_MISS:
    string = "miss";
    break;
  case C_HIT:
    string = "hit";
    break;
  case C_MISS_EVICTION:
    string = "miss eviction";
    break;
  case C_M_M:
    string = "miss miss";
    break;
  case C_M_H:
    string = "miss hit";
    break;
  case C_M_ME:
    string = "miss miss eviction";
    break;
  case C_H_M:
    string = "hit miss";
    break;
  case C_H_H:
    string = "hit hit";
    break;
  case C_H_ME:
    string = "hit miss eviction";
    break;
  case C_ME_H:
    string = "miss eviction hit";
    break;
  case C_ME_M:
    string = "miss eviction miss";
    break;
  case C_ME_ME:
   string = "miss eviction miss eviction";
    break;
  }
  return string;
}

/* returns the value from a slice in an integer - keep in mind the order
15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0

so binary_slice(n, 5, 0) gets you the number in 5 4 3 2 1 0 */
int binary_slice(long long int n, int high, int low){
  int length = high - low + 1;
  unsigned int mask = ((int) pow(2, length) - 1) << low;
  return (mask & n) >> low;
}

/* structure for a cache line - data isn't necessary here */
typedef struct {
  bool valid;
  int tag;
  int last_used;
} cache_line;
/* for arbitrary-length caches, use a linked list. there'll be E lines per set, with 2^b sets */
typedef struct ll_line {
  cache_line line;
  struct ll_line *next;
} ll_line;
/* .. so an arbitrary-length cache is a linked list of sets, themselves linked lists of lines */
typedef struct ll_set {
  ll_line set;
  struct ll_set *next;
} ll_set;

ll_set *ll_set_prepend(ll_set *head, ll_line set){
  ll_set *set_node = malloc(sizeof(ll_set));
  set_node->set = set;
  set_node->next = head;
  head = set_node;
  return head;
}

ll_line *ll_line_prepend(ll_line *head, cache_line line){
  ll_line *line_node = malloc(sizeof(ll_line));
  line_node->line = line;
  line_node->next = head;
  head = line_node;
  return line_node;
}

ll_set *init_cache(int set_index_bits, int assoc, int block_bits){
  //int block_size = (int) pow(block_bits, 2);
  int set_num = (int) pow(2, set_index_bits);
  int i, j;
  ll_set *cache = NULL;
  /* initialize the sets using linked list prepending, set_num in total */
  for (i = 0; i < set_num; i++){
    /* make the set, then init the lines */
    ll_line *set = NULL;
    for (j = 0; j < assoc; j++){
      /* we need -1 as a sentinel value for last_used and tag */
      cache_line line = (cache_line) {.valid = FALSE, .tag = -1, .last_used = 0};
      set = ll_line_prepend(set, line);
    }
    /* now prepend the set */
    cache = ll_set_prepend(cache, *set);
  }
  return cache;
}

ll_line *set_for_addr(long long int addr, ll_set *cache, int s, int b){
  int set_index = binary_slice(addr, b+s-1, b);
  int i;
  for (i = 0; i < set_index; i++){
    cache = cache->next;
  }
  return &cache->set;
}

cache_line *set_search_max(ll_line *set){
  int max = INT_MIN;
  cache_line *line;
  while (set != NULL){
    if (set->line.last_used > max){
      max = set->line.last_used;
      line = &set->line;
    }
    set = set->next;
  }
  return line;
}

/* search for a tag in a set, following LRU rules, returning in order
 - minimum valid line with matching tag, or
 - next invalid line, or
 - minimum valid line */
cache_line *set_search_tag_LRU(ll_line *set, int tag){
  int min = INT_MAX;
  cache_line *line_tag = NULL, *line_min = NULL, *line_invalid = NULL;
  while (set != NULL){
    /* search for min valid line */
    if (set->line.last_used < min && set->line.valid){
      min = set->line.last_used;
      line_min = &set->line;
    }
    /* search for matching tag */
    if (set->line.tag == tag && set->line.valid){
      line_tag = &set->line;
    }
    /* search for any invalid line */
    else if (set->line.valid == FALSE){
      line_invalid = &set->line;
    }
    set = set->next;
  }
  if (line_tag){
    return line_tag;
  }
  else if (line_invalid){
    return line_invalid;
  }
  else {
    return line_min;
  }
}

/* cache modification operations return the hit/miss/eviction state of that operation */
cache_access_state_t mem_load(long long int addr, ll_set *cache, int assoc, int s, int b){
  /* get the right set - for a fully associative cache it'll be just the one, at index 0 */
  ll_line *set = set_for_addr(addr, cache, s, b); 
  /* LRU policy evicts the lowest count line */
  int addr_tag = binary_slice(addr, INT_SIZE-1, b+s);
  cache_line *line = set_search_tag_LRU(set, addr_tag);
  if (line->valid){
    if (line->tag == addr_tag){
      line->last_used = (set_search_max(set)->last_used) + 1;
      return C_HIT;
    }
    else {
      line->tag = addr_tag;
      line->last_used = (set_search_max(set)->last_used) + 1;
      return C_MISS_EVICTION;
    }
  }
  else {
    /* populate info, like during warmup */
    line->tag = addr_tag;
    line->valid = TRUE;
    line->last_used = (set_search_max(set)->last_used) + 1;
    return C_MISS;
  }
}
/* apparently cache access logic is the same for load/store */
cache_access_state_t mem_store(long long int addr, ll_set *cache, int assoc, int s, int b){
  return mem_load(addr, cache, assoc, s, b);
}

cache_access_state_t mem_modify(long long int addr, ll_set *cache, int assoc, int s, int b){
  /* "combine" the cache access states of a load then store */
  cache_access_state_t load = mem_load(addr, cache, assoc, s, b);
  cache_access_state_t store = mem_store(addr, cache, assoc, s, b);
  return combine(load, store);
}
  
void print_help(){
  printf("Usage: ./csim-ref [-hv] -s <s> -E <E> -b <b> -t <tracefile>\n");
}

int main(int argc, char *argv[])
{  
  int setbits = 0;
  int assoc = 0;
  int blockbits = 0;
  char *tracefile_name;
  bool help = FALSE;
  bool verbose = FALSE;

  int opt;
  while ((opt = getopt(argc, argv, "h::vs:E:b:t:")) != -1){
    switch (opt){
    case 'h':
      help = TRUE;
      break;
    case 'v':
      verbose = TRUE;
      break;
    case 's':
      setbits = atoi(optarg);
      break;
    case 'E':
      assoc = atoi(optarg);
      break;
    case 'b':
      blockbits = atoi(optarg);
      break;
    case 't':
      tracefile_name = optarg;
      break;
    }
  }
  long long int addr = 0;
  int count = 0, size = 0, hits = 0, misses = 0, evictions = 0;
  char operation;
  cache_access_state_t state;
  if (help){
    print_help();
  }
  else {
    FILE *trace = fopen(tracefile_name, "r");
    if (!trace){
      printf("Error opening file %s\n", tracefile_name);
    }
    ll_set *cache = init_cache(setbits, assoc, blockbits);    
    while ((count = fscanf(trace, " %c %llx,%d", &operation, &addr, &size)) != EOF){
      /* perform the right memory operation */
      switch (operation){
      case 'M':
	state = mem_modify(addr, cache, assoc, setbits, blockbits);
	break;
      case 'L':
	state = mem_load(addr, cache, assoc, setbits, blockbits);
	break;
      case 'S':
	state = mem_store(addr, cache, assoc, setbits, blockbits);
	break;
      }
      if (operation == 'M' || operation == 'L' || operation == 'S'){
	if (verbose){
	  printf("%c %llx,%d %s\n", operation, addr, size, state_name(state));
	}
      /* tally up */
      switch (state){
      case C_MISS:
	misses += 1;
	break;
      case C_HIT:
	hits += 1;
	break;
      case C_MISS_EVICTION:
	misses += 1;
	evictions += 1;
	break;
      case C_M_M:
	misses += 2;
	break;
      case C_M_H:
	misses += 1;
	hits += 1;
	break;
      case C_M_ME:
	misses += 2;
	evictions += 1;
	break;
      case C_H_H:
	hits += 2;
	break;
      case C_H_M:
	hits += 1;
	misses += 1;
	break;
      case C_H_ME:
	hits += 1;
	misses += 1;
	evictions += 1;
	break;
      case C_ME_H:
	misses += 1;
	evictions += 1;
	hits += 1;
	break;
      case C_ME_M:
	misses += 2;
	evictions += 1;
	break;
      case C_ME_ME:
	misses += 2;
	evictions += 2;
	break;
      }
      }
    }
  } 
  printSummary(hits, misses, evictions);
  return 0;
}
