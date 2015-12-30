#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "glog.h"
#include "gmem.h"
#include "cover.h"

/* How big will the initial bit set allocation be. */
#define COVER_INITIAL_SIZE 8   /* 8 * CHAR_BIT = 64 bits (lines) */

#define COVER_LIST_INITIAL_SIZE 8   /* 8 files in the hash */

/* Handle an array of unsigned char as a bit set. */
#define BIT_TURN_ON(data, bit)   data[bit/CHAR_BIT] |=  (1 << (bit%CHAR_BIT))
#define BIT_TURN_OFF(data, bit)  data[bit/CHAR_BIT] &= ~(1 << (bit%CHAR_BIT))
#define BIT_IS_ON(data, bit)    (data[bit/CHAR_BIT] &   (1 << (bit%CHAR_BIT)))

/* Count of the hash collisions in the hash table */
#ifdef GLOG_SHOW
static unsigned int max_collisions = 0;
#endif

/* Add a line to a given CoverNode; grow its bit set if necessary. */
static void cover_node_set_line(CoverNode* node, int line);

/* Add a node to the list of files */
static CoverNode* add_get_node(CoverList *cover, const char *file);

CoverList* cover_create(void) {
  CoverList* cover;
  GMEM_NEW(cover, CoverList*, sizeof(CoverList));

  cover->used = 0;
  cover->size = COVER_LIST_INITIAL_SIZE;
  GMEM_NEWARR(cover->list, CoverNode**, COVER_LIST_INITIAL_SIZE, sizeof(CoverNode *));

  return cover;
}

void cover_destroy(CoverList* cover) {
  int i;
  CoverNode* node = 0;

  assert(cover);

  for (i = 0; i < cover->size ; i++) {
    node = cover->list[i];
    if (!node) {
      continue;
    }

    CoverNode* tmp = node;
    /* GLOG(("Destroying set for [%s], %d/%d elements", node->file, node->bcnt, node->alen*CHAR_BIT)); */
    /* GLOG(("Destroying string [%s]", tmp->file)); */
    GMEM_DELSTR(tmp->file, -1);
    /* GLOG(("Destroying array [%p] with %d elements", tmp->lines, tmp->alen)); */
    GMEM_DELARR(tmp->lines, unsigned char*, tmp->alen, sizeof(unsigned char*));
    /* GLOG(("Destroying node [%p]", tmp)); */
    GMEM_DEL(tmp, CoverNode*, sizeof(CoverNode));
    cover->list[i] = 0;
  }

  GLOG(("Destroying cover [%p]. Max run %d. Used: %d", cover, max_collisions, cover->used));
  GMEM_DELARR(cover->list, CoverNode**, cover->size, sizeof(CoverNode *));
  GMEM_DEL(cover, CoverList*, sizeof(CoverList));
}

CoverNode* cover_add(CoverList* cover, const char* file, int line) {
  CoverNode* node = 0;

  assert(cover);
  node = add_get_node(cover, file);

  assert(node);
  cover_node_set_line(node, line);

  return node;
}

void cover_dump(CoverList* cover, FILE* fp) {
  CoverNode* node = 0;
  int ncount = 0, i = 0;

  assert(cover);

  /*
   * We output the cover data as elements in a JSON hash
   * that must be opened / closed OUTSIDE this routine.
   */
  fprintf(fp, "\"files\":{");
  for (i = 0 ; i < cover->size; i++) {
    int j = 0;
    int lcount = 0;
    node = cover->list[i];
    if (!node) {
      continue;
    }

    if (ncount++) {
      fprintf(fp, ",");
    }
    fprintf(fp, "\"%s\":{",
            node->file);
    for (j = 0; j < node->bmax; ++j) {
      if (BIT_IS_ON(node->lines, j)) {
        /* TODO: maybe output more than one line in each line with type 2? */
        if (lcount++) {
          fprintf(fp, ",");
        }
        fprintf(fp, "\"%d\":%d", j+1, 1);
      }
    }
    fprintf(fp, "}");
  }
  fprintf(fp, "}");
}

static void cover_node_set_line(CoverNode* node, int line) {
  /* keep track of largest line seen so far */
  if (node->bmax < line) {
    node->bmax = line;
  }

  --line; /* store line numbers zero-based */

  /* maybe we need to grow the bit set? */
  int needed = line / CHAR_BIT + 1;
  if (node->alen < needed) {
    /* start at COVER_INITIAL_SIZE, then duplicate the size, until we have */
    /* enough room */
    int size = node->alen ? node->alen : COVER_INITIAL_SIZE;
    while (size < needed) {
      size *= 2;
    }

    /* GLOG(("Growing map for [%s] from %d to %d - %p", node->file, node->alen, size, node->lines)); */

    /* realloc will grow the data and keep all current values... */
    GMEM_REALLOC(node->lines, unsigned char*, node->alen * sizeof(unsigned char*), size * sizeof(unsigned char*));

    /* ... but it will not initialise the new space to 0. */
    memset(node->lines + node->alen, 0, size - node->alen);

    /* we are bigger now */
    node->alen = size;
  }

  /* if the line was not already registered, do so and keep track of how many */
  /* lines we have seen so far */
  if (! BIT_IS_ON(node->lines, line)) {
    /* GLOG(("Adding line %d for [%s]", line, node->file)); */
    ++node->bcnt;
    BIT_TURN_ON(node->lines, line);
  }
}

static U32 find_pos(CoverNode **where, U32 hash, const char *file, int size) {
  U32 pos = hash % size;

#ifdef GLOG_SHOW
  unsigned int run = 0;
#endif

  while (where[pos] &&
         hash != where[pos]->hash &&
         strcmp(file, where[pos]->file) != 0) {
    pos = (pos + 1) % size;

#ifdef GLOG_SHOW
    ++run;
#endif
  }

#ifdef GLOG_SHOW
  if (run > max_collisions) {
    max_collisions = run;
  }
#endif

  return pos;
}

static CoverNode* add_get_node(CoverList *cover, const char *file) {
  U32 hash, pos, i;
  CoverNode *node = NULL, **new_list = NULL;
  ssize_t len = strlen(file);

  /* TODO: comment these magic numbers */
  /* TODO: move this enlargement code to a separate function */
  if (3 * cover->used > 2 * cover->size) {
    GMEM_NEWARR(new_list, CoverNode**, cover->size * 2, sizeof(CoverNode *));
    for (i = 0; i < cover->size; i++) {
      if (!cover->list[i]) {
        continue;
      }
      pos = find_pos(new_list, cover->list[i]->hash, cover->list[i]->file, cover->size * 2);
      new_list[pos] = cover->list[i];
    }

    GMEM_DELARR(cover->list, CoverNode**, cover->size, sizeof(CoverNode *));
    cover->list = new_list;
    cover->size *= 2;
  }

  /* Compute hash value for file name using Perl's hash function */
  PERL_HASH(hash, file, len);

  pos = find_pos(cover->list, hash, file, cover->size);
  if (cover->list[pos]) {
    return cover->list[pos];
  }

  GMEM_NEW(node, CoverNode*, sizeof(CoverNode));
  /* TODO: normalise name first? ./foo.pl, foo.pl, ../bar/foo.pl, etc. */
  int l = 0;
  GMEM_NEWSTR(node->file, file, -1, l);
  node->lines = NULL;
  node->hash = hash;
  node->alen = node->bcnt = node->bmax = 0;

  ++cover->used;
  cover->list[pos] = node;

  /* GLOG(("Adding set for [%s]", node->file)); */
  return node;
}
