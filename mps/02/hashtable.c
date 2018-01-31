#include <stdlib.h>
#include <string.h>
#include "hashtable.h"

/* Daniel J. Bernstein's "times 33" string hash function, from comp.lang.C;
   See https://groups.google.com/forum/#!topic/comp.lang.c/lSKWXiuNOAk */
unsigned long hash(char *str) {
  unsigned long hash = 5381;
  int c;

  while ((c = *str++))
    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

  return hash;
}

hashtable_t *make_hashtable(unsigned long size) {
  hashtable_t *ht = malloc(sizeof(hashtable_t));
  ht->size = size;
  ht->buckets = calloc(sizeof(bucket_t *), size);
  /* pointers were set to null by calloc */
  return ht;
}

void ht_put(hashtable_t *ht, char *key, void *val) {
  unsigned int idx = hash(key) % ht->size;
  bucket_t *cur_b = ht->buckets[idx];
  while (cur_b){
    if (strcmp(cur_b->key, key) == 0){
      /* update entry */
      free(cur_b->key);
      free(cur_b->val);
      cur_b->val = val;
      cur_b->key = key;
      return;
    }
    cur_b = cur_b->next;
  }
  /* didn't update, so make a new bucket*/
  bucket_t *new_b = malloc(sizeof(bucket_t));
  new_b->key = key;
  new_b->val = val;
  /* prepend */
  new_b->next = ht->buckets[idx];
  ht->buckets[idx] = new_b;
}

void *ht_get(hashtable_t *ht, char *key) {
  unsigned int idx = hash(key) % ht->size;
  bucket_t *b = ht->buckets[idx];
  while (b) {
    if (strcmp(b->key, key) == 0) {
      return b->val;
    }
    b = b->next;
  }
  return NULL;
}

void ht_iter(hashtable_t *ht, int (*f)(char *, void *)) {
  bucket_t *b;
  unsigned long i;
  for (i=0; i<ht->size; i++) {
    b = ht->buckets[i];
    while (b) {
      if (!f(b->key, b->val)) {
        return ; // abort iteration
      }
      b = b->next;
    }
  }
}

void free_hashtable(hashtable_t *ht) {
  unsigned long i;
  bucket_t *b;
  bucket_t *prev_b;
  for (i = 0; i < ht->size; i++){
    b = ht->buckets[i];
    prev_b = ht->buckets[i];
    while (b){
      prev_b = b;
      b = b->next;
      free(prev_b->key);
      free(prev_b->val);
      free(prev_b);
    }
  }
  free(ht->buckets);
  free(ht);
}

void ht_del(hashtable_t *ht, char *key) {
  unsigned int idx = hash(key) % ht->size;
  bucket_t *b = ht->buckets[idx];
  bucket_t *prev_b = ht->buckets[idx];
  while (b){
    if (strcmp(b->key, key) == 0){
      free(b->key);
      free(b->val);
      /*special case for head element */
      if (b == ht->buckets[idx]){
	ht->buckets[idx] = b->next;
      }
      else {
	/*fix the list by making prev_b point to b's next */
	prev_b->next = b->next;
      }
      free(b);
      return;
    }
    prev_b = b;
    b = b->next;
  }
}

void ht_rehash(hashtable_t *ht, unsigned long newsize) {
  hashtable_t *new_ht = make_hashtable(newsize);
  /* there's some contention about free(NULL); standards-wise, free(NULL) is NOP but I use it here so I don't have to repeat the entirety of free_hashtable. */
  /* put old keys into new-sized bucket */
  unsigned long i;
  bucket_t *b;
  for (i=0; i<ht->size; i++) {
    b = ht->buckets[i];
    while (b){
      ht_put(new_ht, b->key, b->val);
      b->val = NULL;
      b->key = NULL;
      b = b->next;
    }
  }
  /* temp pointer for the old buckets */
  bucket_t **temp_buckets = ht->buckets;
  /* swap the new and old buckets & sizes */
  ht->buckets = new_ht->buckets;
  new_ht->size = ht->size;
  ht->size = newsize;
  new_ht->buckets = temp_buckets;
  /* now we can free the 'new' one with the old one's buckets, and the old one has new buckets */
  free_hashtable(new_ht);
}
