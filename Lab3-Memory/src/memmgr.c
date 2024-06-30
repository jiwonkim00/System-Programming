//--------------------------------------------------------------------------------------------------
// System Programming                       Memory Lab                                   Spring 2024
//
/// @file
/// @brief dynamic memory manager
/// @author Jiwon Kim
/// @studid 2019-11563
//--------------------------------------------------------------------------------------------------


// Dynamic memory manager
// ======================
// This module implements a custom dynamic memory manager.
//
// Heap organization:
// ------------------
// The data segment for the heap is provided by the dataseg module. A 'word' in the heap is
// eight bytes.
//
// Implicit free list:
// -------------------
// - minimal block size: 32 bytes (header + footer + 2 data words)
// - h,f: header/footer of fre e block
// - H,F: header/footer of allocated block
//
// - state after initialization
//
//         initial sentinel half-block                  end sentinel half-block
//                   |                                             |
//   ds_heap_start   |   heap_start                         heap_end       ds_heap_brk
//               |   |   |                                         |       |
//               v   v   v                                         v       v
//               +---+---+-----------------------------------------+---+---+
//               |???| F | h :                                 : f | H |???|
//               +---+---+-----------------------------------------+---+---+
//                       ^                                         ^
//                       |                                         |
//               32-byte aligned                           32-byte aligned
//
// - allocation policy: best fit
// - block splitting: always at 32-byte boundaries
// - immediate coalescing upon free
//
// Explicit free list:
// -------------------
// - minimal block size: 32 bytes (header + footer + next + prev)
// - h,f: header/footer of free block
// - n,p: next/previous pointer
// - H,F: header/footer of allocated block
//
// - state after initialization
//
//         initial sentinel half-block                  end sentinel half-block
//                   |                                             |
//   ds_heap_start   |   heap_start                         heap_end       ds_heap_brk
//               |   |   |                                         |       |
//               v   v   v                                         v       v
//               +---+---+-----------------------------------------+---+---+
//               |???| F | h : n : p :                         : f | H |???|
//               +---+---+-----------------------------------------+---+---+
//                       ^                                         ^
//                       |                                         |
//               32-byte aligned                           32-byte aligned
//
// - allocation policy: best fit
// - block splitting: always at 32-byte boundaries
// - immediate coalescing upon free
//

#define _GNU_SOURCE

#include <assert.h>
#include <error.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dataseg.h"
#include "memmgr.h"


/// @name global variables
/// @{
static void *ds_heap_start = NULL;                     ///< physical start of data segment
static void *ds_heap_brk   = NULL;                     ///< physical end of data segment
static void *heap_start    = NULL;                     ///< logical start of heap
static void *heap_end      = NULL;                     ///< logical end of heap
static int  PAGESIZE       = 0;                        ///< memory system page size
static void *(*get_free_block)(size_t) = NULL;         ///< get free block for selected allocation policy
static size_t CHUNKSIZE    = 1<<16;                    ///< minimal data segment allocation unit
static size_t SHRINKTHLD   = 1<<14;                    ///< threshold to shrink heap
static int  mm_initialized = 0;                        ///< initialized flag (yes: 1, otherwise 0)
static int  mm_loglevel    = 0;                        ///< log level (0: off; 1: info; 2: verbose)

// Freelist
static FreelistPolicy freelist_policy  = 0;            ///< free list management policy

static void* exp_freelist_head = NULL;

//
// TODO: add more global variables as needed
//
/// @}

/// @name Macro definitions
/// @{
#define MAX(a, b)          ((a) > (b) ? (a) : (b))     ///< MAX function

#define TYPE               unsigned long               ///< word type of heap
#define TYPE_SIZE          sizeof(TYPE)                ///< size of word type

#define ALLOC              1                           ///< block allocated flag
#define FREE               0                           ///< block free flag
#define STATUS_MASK        ((TYPE)(0x7))               ///< mask to retrieve flags from header/footer
#define SIZE_MASK          (~STATUS_MASK)              ///< mask to retrieve size from header/footer

#define BS                 32                          ///< minimal block size. Must be a power of 2
#define BS_MASK            (~(BS-1))                   ///< alignment mask

#define WORD(p)            ((TYPE)(p))                 ///< convert pointer to TYPE
#define PTR(w)             ((void*)(w))                ///< convert TYPE to void*

#define PREV_PTR(p)        ((p)-TYPE_SIZE)             ///< get pointer to word preceeding p
#define NEXT_PTR(p)        ((p)+TYPE_SIZE)             ///< get pointer to word succeeding p

#define PREV_PREV_PTR(p)   ((p)- 2 * TYPE_SIZE)
#define NEXT_NEXT_PTR(p)   ((p)+ 2 * TYPE_SIZE)

#define HDR2FTR(p)         ((p)+GET_SIZE(p)-TYPE_SIZE) ///< get footer for given header
#define FTR2HDR(p)         ((p)-GET_SIZE(p)+TYPE_SIZE) ///< get header for given footer

#define PACK(size,status)  ((size) | (status))         ///< pack size & status into boundary tag
#define SIZE(v)            (v & SIZE_MASK)             ///< extract size from boundary tag
#define STATUS(v)          (v & STATUS_MASK)           ///< extract status from boundary tag

#define PUT(p, v)          (*(TYPE*)(p) = (TYPE)(v))   ///< write word v to *p
#define GET(p)             (*(TYPE*)(p))               ///< read word at *p
#define GET_SIZE(p)        (SIZE(GET(p)))              ///< extract size from header/footer
#define GET_STATUS(p)      (STATUS(GET(p)))            ///< extract status from header/footer

#define DEBUG 1

#define NEXT_LIST_GET(p)   GET(NEXT_PTR(p))     //get 'next' pointer in exp_free_list
#define PREV_LIST_GET(p)   GET(NEXT_NEXT_PTR(p))   //get 'prev' pointer in exp_free_list

#define NEXT_LIST_SET(bp, new_next)    PUT(NEXT_PTR(bp), new_next)
#define PREV_LIST_SET(bp, new_prev)    PUT(NEXT_NEXT_PTR(bp), new_prev)

#define ALIGNMENT 32  
#define ALIGN(size) (((size + 2 * TYPE_SIZE - 1) >> 5) + 1) << 5

//#define HDRP(bp)         ((void *)(bp) - TYPE_SIZE)  // Header pointer
#define PREV_BLKP(p)    ((p) - GET_SIZE(PREV_PTR(p)))
#define NEXT_BLKP(p)    ((p) + GET_SIZE(p))  // Next block pointer

//
// TODO: add more macros as needed
//
/// @}


/// @name Logging facilities
/// @{

/// @brief print a log message if level <= mm_loglevel. The variadic argument is a printf format
///        string followed by its parametrs
#ifdef DEBUG
  #define LOG(level, ...) mm_log(level, __VA_ARGS__)

/// @brief print a log message. Do not call directly; use LOG() instead
/// @param level log level of message.
/// @param ... variadic parameters for vprintf function (format string with optional parameters)
static void mm_log(int level, ...)
{
  if (level > mm_loglevel) return;

  va_list va;
  va_start(va, level);
  const char *fmt = va_arg(va, const char*);

  if (fmt != NULL) vfprintf(stdout, fmt, va);

  va_end(va);

  fprintf(stdout, "\n");
}

#else
  #define LOG(level, ...)
#endif

/// @}


/// @name Program termination facilities
/// @{

/// @brief print error message and terminate process. The variadic argument is a printf format
///        string followed by its parameters
#define PANIC(...) mm_panic(__func__, __VA_ARGS__)

/// @brief print error message and terminate process. Do not call directly, Use PANIC() instead.
/// @param func function name
/// @param ... variadic parameters for vprintf function (format string with optional parameters)
static void mm_panic(const char *func, ...)
{
  va_list va;
  va_start(va, func);
  const char *fmt = va_arg(va, const char*);

  fprintf(stderr, "PANIC in %s%s", func, fmt ? ": " : ".");
  if (fmt != NULL) vfprintf(stderr, fmt, va);

  va_end(va);

  fprintf(stderr, "\n");

  exit(EXIT_FAILURE);
}
/// @}


static void* bf_get_free_block_implicit(size_t size);
static void* bf_get_free_block_explicit(size_t size);
void free_from_free_list(void* bp);
void add_to_free_list(void* bp);
void* split(void* bp);
void* exp_coalesce(void *bp);
void* imp_coalesce(void *bp);

void mm_init(FreelistPolicy fp)
{
  //LOG(1, "mm_init()");

  //
  // set free list policy
  //
  freelist_policy = fp;
  switch (freelist_policy)
  {
    case fp_Implicit:
      get_free_block = bf_get_free_block_implicit;
      break;
      
    case fp_Explicit:
      get_free_block = bf_get_free_block_explicit;
      break;
    
    default:
      PANIC("Non supported freelist policy.");
      break;
  }

  //
  // retrieve heap status and perform a few initial sanity checks
  //
  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
  PAGESIZE = ds_getpagesize();

  LOG(2, "  ds_heap_start:          %p\n"
         "  ds_heap_brk:            %p\n"
         "  PAGESIZE:               %d\n",
         ds_heap_start, ds_heap_brk, PAGESIZE);

  if (ds_heap_start == NULL) PANIC("Data segment not initialized.");
  if (ds_heap_start != ds_heap_brk) PANIC("Heap not clean.");
  if (PAGESIZE == 0) PANIC("Reported pagesize == 0.");

  //
  // initialize heap
  //
  // TODO

  //if (mm_initialized) return;
  
  // if (ds_heap_start == (void *) -1) {
  //     assert(0);  // Handle sbrk failure
  // }

  if (ds_sbrk(CHUNKSIZE) != ds_heap_brk) PANIC("ds_sbrk error");
  //ds_sbrk(CHUNKSIZE);
  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
  
  heap_start = ds_heap_start + BS;
  heap_end = ds_heap_brk - BS;

  //initial sentinel half block
  PUT(PREV_PTR(heap_start), 0x1);
  //end sentinel half block
  PUT(heap_end, 0x1);

  size_t size = CHUNKSIZE - 2 * BS;

  PUT(heap_start, PACK(size, FREE));  // header
  PUT(HDR2FTR(heap_start), PACK(size, FREE));  // footer

  if (freelist_policy == fp_Explicit) {
    exp_freelist_head = heap_start;

    // next and prev pointers
    NEXT_LIST_SET(heap_start, NULL);
    PREV_LIST_SET(heap_start, NULL); 
  } 

  mm_initialized = 1;
}

static int mm_sbrk(){
  //LOG(2, "mm_sbrk()");

  void* old_heap_end = heap_end;

  if(ds_sbrk(CHUNKSIZE) != ds_heap_brk){
    return -1;
  }

  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);

  heap_end = ds_heap_brk - BS;
  PUT(heap_end, 0x1);   //end sentinel

  if(GET_STATUS(PREV_PTR(old_heap_end)) == FREE){  // old_heap_end is free
    // need to coalesce 
    //LOG(2, "mm_sbrk() - coalesce with last free block");
    void* old_ftr = PREV_PTR(old_heap_end);
    size_t new_size = GET_SIZE(old_ftr) + CHUNKSIZE;
    //LOG(2, "mm_sbrk() - size : %d\n", new_size);

    PUT(FTR2HDR(old_ftr), PACK(new_size, FREE));    // header 
    PUT(PREV_PTR(heap_end), PACK(new_size, FREE));    // footer

  }else{
    PUT(old_heap_end, PACK(CHUNKSIZE, FREE)); // rest of free heap
    PUT(PREV_PTR(heap_end), PACK(CHUNKSIZE, FREE));
  }

  return 1;
}


/// @brief find and return a free block of at least @a size bytes (best fit)
/// @param size size of block (including header & footer tags), in bytes
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* bf_get_free_block_implicit(size_t size)
{
  LOG(1, "bf_get_free_block_implicit(0x%lx (%lu))", size, size);

  assert(mm_initialized);

  //
  // TODO
  //
  void *best_fit = NULL;
  size_t smallest_diff = (TYPE) - 1;  // max size

  void *bp;
  for (bp = heap_start; GET_SIZE(bp) > 0; bp = NEXT_BLKP(bp)) {
    size_t block_size = GET_SIZE(bp);
    if ( !GET_STATUS(bp) && (block_size >= size) ) {  // free && large enough
        size_t diff = block_size - size;
        if (diff == 0) {
          best_fit = bp;
          break; 
        }
        if (diff < smallest_diff) {  // set smallest difference
          best_fit = bp;
          smallest_diff = diff;
        }
    }
  }

  if (best_fit == NULL) {
    // cannot find appropriate free block
      //(2, "get_free_imp() - need sbrk");

    if (mm_sbrk() == -1) return NULL;
    return bf_get_free_block_implicit(size);
  }

  return best_fit;  
}


/// @brief find and return a free block of at least @a size bytes (best fit)
/// @param size size of block (including header & footer tags), in bytes
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* bf_get_free_block_explicit(size_t size)
{
  LOG(1, "bf_get_free_block_explicit(0x%lx (%lu))", size, size);

  assert(mm_initialized);
  
  //
  // TODO
  //
  void *best_fit = NULL;
  TYPE smallest_diff = (TYPE) - 1;  // max size
  void *bp;

  for (bp = exp_freelist_head; bp != NULL; bp = NEXT_LIST_GET(bp)) {
    TYPE current_size = GET_SIZE(bp);
    if ( (GET_STATUS(bp) == FREE) && current_size >= size) {  // free && large enough
      
      TYPE diff = current_size - size;
      if (diff == 0) {
        best_fit = bp;
        smallest_diff = 0;
        break;
      }
      if (diff < smallest_diff) {  // set smallest difference
        best_fit = bp;
        smallest_diff = diff;
      }
    }
  }

  //(1, "best fit: %p\n", best_fit);
  //LOG(1, "diff: %d\n", smallest_diff);


  if (best_fit == NULL) {
    // cannot find appropriate free block
    //LOG(1, "get_free_exp() - need sbrk");
    
    if (mm_sbrk() == -1) return NULL;

    //LOG(1, "bf_get_free_block_explicit 2");
    
    return bf_get_free_block_explicit(size);
  }

  return best_fit;  
}


void* mm_malloc(size_t size)
{
  LOG(1, "mm_malloc(0x%lx (%lu))", size, size);
  //LOG(1, "mm_malloc(0x%lx (%lu))", size, size);


  assert(mm_initialized);

  //
  // TODO
  //
  if (size == 0) {
      return NULL;
  }

  // Adjust block size for alignment
  size_t adjusted_size = ALIGN(size); 
  if (adjusted_size < BS) adjusted_size = BS;  
  //LOG(2, "mm_malloc() - adjusted size:%d\n", adjusted_size);
  
  void *bp = get_free_block(adjusted_size);

  if (bp == NULL) return NULL;  // bp = pointer to header of block

  size_t block_size = GET_SIZE(bp);

  if (block_size >= adjusted_size + BS) {
    // Case 1: Split the block
    //(2, "mm_malloc() - need to split");
    size_t remaining_size = block_size - adjusted_size;

    PUT(bp, PACK(adjusted_size, ALLOC));  //allocated block header
    PUT(HDR2FTR(bp), PACK(adjusted_size, ALLOC)); //footer

    void *next_bp = NEXT_BLKP(bp);  // (splitted) next free block
    PUT(next_bp, PACK(remaining_size, FREE));  //header
    PUT(HDR2FTR(next_bp), PACK(remaining_size, FREE));  //footer

    //LOG(2, "mm_malloc() - bp: %p, splitted bp: %p\n", bp, next_bp);
    //LOG(2, "mm_malloc() - allocated size: %d, remaining size: %d\n", adjusted_size, remaining_size);

    // need adjustment of pointers under explicit policy
    if (freelist_policy == fp_Explicit) {
      //adjust pointers for splitting in exp free list
      split(bp);

    }
  } else if((block_size - adjusted_size) != 0) {
    PANIC("Payload Alignment error");

  } else {
    // Case 2: No split
    PUT(bp, PACK(block_size, ALLOC)); //header
    PUT(HDR2FTR(bp), PACK(block_size, ALLOC));  //footer

    if (freelist_policy == fp_Explicit) {
      free_from_free_list(bp);
    }
  }
  
  // return NEXT_PTR(bp);
  return NEXT_PTR(bp);
  // if (freelist_policy == fp_Implicit) return NEXT_PTR(bp);
  // return NEXT_NEXT_PTR(NEXT_PTR(bp));
}

void* split(void* bp) {
  //LOG(2, "split()");

  void* next_ptr = NEXT_LIST_GET(bp);
  void* prev_ptr = PREV_LIST_GET(bp);
  void *new_bp = NEXT_BLKP(bp);

  //adjust pointers prev, next block pointers
  if (prev_ptr == NULL) { //this is the first block in free list
    exp_freelist_head = new_bp;
    //LOG(2, "split() - point to new bp: %p\n", new_bp);
  }else {
    NEXT_LIST_SET(prev_ptr, new_bp);
  }

  if (next_ptr != NULL) { //no next block
    PREV_LIST_SET(next_ptr, new_bp);
  }
  
  //set pointers of new free block
  NEXT_LIST_SET(new_bp, next_ptr);
  PREV_LIST_SET(new_bp, prev_ptr);

}


void* mm_calloc(size_t nmemb, size_t size)
{
  LOG(1, "mm_calloc(0x%lx, 0x%lx (%lu))", nmemb, size, size);

  assert(mm_initialized);

  //
  // calloc is simply malloc() followed by memset()
  //
  void *payload = mm_malloc(nmemb * size);

  if (payload != NULL) memset(payload, 0, nmemb * size);

  return payload;
}

void* imp_coalesce(void *bp) {
  //LOG(2, "imp_coalesce()");

  void* prev_blk = PREV_BLKP(bp);
  void* next_blk = NEXT_BLKP(bp);
  size_t prev_alloc = GET_STATUS(prev_blk) == ALLOC;
  size_t next_alloc = GET_STATUS(next_blk) == ALLOC;
  
  size_t size = GET_SIZE(bp);
  void* new_hdr;

  if (prev_alloc && next_alloc) {  // Case 1: No coalescing
      new_hdr = bp;
      
  } else if (!prev_alloc && next_alloc) {  // Case 2: Coalesce with previous
      new_hdr = prev_blk;
      size += GET_SIZE(prev_blk);

  } else if (prev_alloc && !next_alloc) {  // Case 3: Coalesce with next
      new_hdr = bp;
      size += GET_SIZE(next_blk);

  } else {  // Case 4: Coalesce both
      new_hdr = prev_blk;
      size += (GET_SIZE(prev_blk) + GET_SIZE(next_blk));
  }
  
  //adjust header, footer
  PUT(new_hdr, PACK(size, FREE));  //header
  PUT(HDR2FTR(new_hdr), PACK(size, FREE));   //footer

  //LOG(2, "imp_coalesce size : %lu\n", size);
  return new_hdr;
}

void* exp_coalesce(void *bp) {
  //LOG(2, "exp_coalesce()");

  void* prev_blk = PREV_BLKP(bp);
  void* next_blk = NEXT_BLKP(bp);

  size_t prev_alloc = GET_STATUS(prev_blk) == ALLOC;
  size_t next_alloc = GET_STATUS(next_blk) == ALLOC;
  size_t size = GET_SIZE(bp);

  void* new_hdr;

  if (prev_alloc && next_alloc) {  // Case 1: No coalescing

      new_hdr = bp;
      //LOG(2, "no exp_coalesce needed, size: %d\n", size);

  } else if (!prev_alloc && next_alloc) {  // Case 2: Coalesce with previous      

      new_hdr = prev_blk;
      free_from_free_list(prev_blk);

      size += GET_SIZE(prev_blk);
      //adjust header, footer
      PUT(new_hdr, PACK(size, FREE));  //header
      PUT(HDR2FTR(new_hdr), PACK(size, FREE));   //footer

      //LOG(2, "exp_coalesce size with previous, size: %d\n", size);


  } else if (prev_alloc && !next_alloc) {  // Case 3: Coalesce with next

      new_hdr = bp;
      free_from_free_list(next_blk);

      size += GET_SIZE(next_blk);
      //adjust header, footer
      PUT(new_hdr, PACK(size, FREE));  //header
      PUT(HDR2FTR(new_hdr), PACK(size, FREE));   //footer

      //LOG(2, "exp_coalesce size with next, size: %d\n", size);

  
  } else {  // Case 4: Coalesce both

      new_hdr = prev_blk;
      free_from_free_list(prev_blk);
      free_from_free_list(next_blk);
      
      size += GET_SIZE(prev_blk) + GET_SIZE(next_blk);
      PUT(new_hdr, PACK(size, FREE));
      PUT(HDR2FTR(new_hdr), PACK(size, FREE));

      //LOG(2, "exp_coalesce size with both, size: %d\n", size);

  }
  PUT(new_hdr, PACK(size, FREE));  //header
  PUT(HDR2FTR(new_hdr), PACK(size, FREE));   //footer
  
  //LOG(2, "exp_coalesce size : %lu\n", size);
  return new_hdr;
}

void free_from_free_list(void* bp) {
  //LOG(2, "free_from_free_list()");

  void *next_bp = NEXT_LIST_GET(bp);  // Get the next block in the list
  void *prev_bp = PREV_LIST_GET(bp);  // Get the previous block in the list
  // If the block is the head of the list, update the head pointer
  if (bp == exp_freelist_head) {
      exp_freelist_head = next_bp;
      //PREV_LIST_SET(next_bp, NULL);
  }

  // Update the next pointer of the previous block, if it exists
  if (prev_bp != NULL) {
      NEXT_LIST_SET(prev_bp, next_bp);
  }

  // Update the previous pointer of the next block, if it exists
  if (next_bp != NULL) {
      PREV_LIST_SET(next_bp, prev_bp);
  }

  // Optionally clear the next and previous pointers of freed block
  NEXT_LIST_SET(bp, NULL);
  PREV_LIST_SET(bp, NULL);
}

void add_to_free_list(void *bp) {

  // ensure that it's free
  // PUT(bp, PACK(GET_SIZE(bp), FREE)); 
  // PUT(HDR2FTR(bp), PACK(GET_SIZE(bp), FREE));

  void* old_head = exp_freelist_head;
  //LOG(2, "add_to_free_list, old head: %p\n", old_head);

  NEXT_LIST_SET(bp, old_head);
  PREV_LIST_SET(bp, NULL);  

  if (exp_freelist_head != NULL) PREV_LIST_SET(old_head, bp);      //modify old head prev
  exp_freelist_head = bp; //insert at front
}

void* mm_realloc(void *ptr, size_t size)
{
  LOG(1, "mm_realloc(%p, 0x%lx (%lu))", ptr, size, size);

  assert(mm_initialized);

  //
  // TODO
  //

  if (ptr == NULL) {
    return mm_malloc(size);
  }
  if (size == 0) {
    // Equivalent to free
    mm_free(ptr);
    return NULL;
  }

  void* cur = PREV_PTR(ptr);  //get header
  void* next_blk = NEXT_BLKP(cur);
  void* prev_blk = PREV_BLKP(cur);
  
  size_t block_size = GET_SIZE(cur);
  size_t new_size = ALIGN(size);
  if (new_size < BS) new_size = BS;  
  

  size_t next_size = GET_SIZE(next_blk);
  size_t total_size = block_size + next_size;
  size_t next_free = GET_STATUS(next_blk) == FREE;

  size_t remaining_size;
  void* splitted_bp;

  void* return_ptr = ptr;

  if (new_size + BS <= block_size) {
    // Case 1: need splitting

    remaining_size = block_size - new_size;

    PUT(cur, PACK(new_size, ALLOC));  //allocated block header
    PUT(HDR2FTR(cur), PACK(new_size, ALLOC)); //footer

    splitted_bp = NEXT_BLKP(cur);  // (splitted) next free block
    PUT(splitted_bp, PACK(remaining_size, FREE));  //header
    PUT(HDR2FTR(splitted_bp), PACK(remaining_size, FREE));  //footer

    //check for coalescing
    if (freelist_policy == fp_Implicit) {
      imp_coalesce(splitted_bp);

    } else {  //fp_Explicit
      exp_coalesce(splitted_bp);  //will not coalesce with prev, header stays the same
      add_to_free_list(splitted_bp); 
    }

  } else if (new_size <= block_size) {
    // Case 2: new size < block size without splitting
    PUT(cur, PACK(new_size, ALLOC)); //header
    PUT(HDR2FTR(cur), PACK(new_size, ALLOC));  //footer
    // just adjust header, footer for size

  } else if (next_free && (total_size >= new_size)){  // new size > block size
    // Case 3: next block is free && large enough to merge
    
    if (freelist_policy == fp_Explicit) free_from_free_list(next_blk);

    if (new_size + BS <= total_size) {
      // Case 3-1: need to split
      remaining_size = total_size - new_size;

      PUT(cur, PACK(new_size, ALLOC));  //header
      PUT(HDR2FTR(cur), PACK(new_size, ALLOC));   //footer

      splitted_bp = NEXT_BLKP(cur);  // (splitted) next free block
      PUT(splitted_bp, PACK(remaining_size, FREE));  //header
      PUT(HDR2FTR(splitted_bp), PACK(remaining_size, FREE));  //footer

      if (freelist_policy == fp_Explicit) add_to_free_list(splitted_bp); 

    } else {
      // Case 3-2: no need to split
      PUT(cur, PACK(new_size, ALLOC));  //header
      PUT(HDR2FTR(cur), PACK(new_size, ALLOC));  //footer
    }

  } else {
    // Case 4: next is not free || total_size not big enough
    // look for other block in the freelist
    
    return_ptr = mm_malloc(size);
    if (return_ptr == NULL) {
        return NULL;  // Allocation failed
    }
    memcpy(return_ptr, cur, block_size - 2*TYPE_SIZE);  // Copy the old data
    mm_free(cur);  // Free the old block
  }

  return return_ptr;
}


void mm_free(void *ptr)
{
  LOG(1, "mm_free(%p)", ptr);
  // LOG(1, "mm_free(%p)", ptr);
  // LOG(1, "mm_free(%p)", ptr);

  assert(mm_initialized);

  //
  // TODO
  //
  //LOG(1, "mm_free");
  if (ptr == NULL) return;

  void* bp = PREV_PTR(ptr);  // get block header
  // if (freelist_policy == fp_Implicit) {bp = PREV_PTR(ptr);}
  // else {bp = PREV_PREV_PTR(PREV_PTR(ptr));}

  //LOG(2, "mm_free(): block header %p\n", bp);

  // Error check: double free
  if (GET_STATUS(bp) == FREE) {
      PANIC("Double free detected \n");
      return;
  }

  // Coalesce & set block as free 
  // - also take care of packing headers and footers inside the function
  void* coalesced_free_blk;
  if (freelist_policy == fp_Implicit) {
    imp_coalesce(bp);
    
  } else {
    coalesced_free_blk = exp_coalesce(bp);
    add_to_free_list(coalesced_free_blk);   // add block to free list, pointer adjustments
  }
}


void mm_setloglevel(int level)
{
  mm_loglevel = level;
}


void mm_check(void)
{
  assert(mm_initialized);

  void *p;

  char *fpstr;
  if (freelist_policy == fp_Implicit) fpstr = "Implicit";
  else if (freelist_policy == fp_Explicit) fpstr = "Explicit";
  else fpstr = "invalid";

  printf("----------------------------------------- mm_check ----------------------------------------------\n");
  printf("  ds_heap_start:          %p\n", ds_heap_start);
  printf("  ds_heap_brk:            %p\n", ds_heap_brk);
  printf("  heap_start:             %p\n", heap_start);
  printf("  heap_end:               %p\n", heap_end);
  printf("  free list policy:       %s\n", fpstr);

  printf("\n");
  p = PREV_PTR(heap_start);
  printf("  initial sentinel:       %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  p = heap_end;
  printf("  end sentinel:           %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  printf("\n");

  if(freelist_policy == fp_Implicit){
    printf("    %-14s  %8s  %10s  %10s  %8s  %s\n", "address", "offset", "size (hex)", "size (dec)", "payload", "status");
  }
  else if(freelist_policy == fp_Explicit){
    printf("    %-14s  %8s  %10s  %10s  %8s  %-14s  %-14s  %s\n", "address", "offset", "size (hex)", "size (dec)", "payload", "next", "prev", "status");
  }

  long errors = 0;
  p = heap_start;
  while (p < heap_end) {
    char *ofs_str, *size_str;

    TYPE hdr = GET(p);
    TYPE size = SIZE(hdr);
    TYPE status = STATUS(hdr);

    void *next = NEXT_LIST_GET(p);
    void *prev = PREV_LIST_GET(p);

    if (asprintf(&ofs_str, "0x%lx", p-heap_start) < 0) ofs_str = NULL;
    if (asprintf(&size_str, "0x%lx", size) < 0) size_str = NULL;

    if(freelist_policy == fp_Implicit){
      printf("    %p  %8s  %10s  %10ld  %8ld  %s\n",
                p, ofs_str, size_str, size, size-2*TYPE_SIZE, status == ALLOC ? "allocated" : "free");
    }
    else if(freelist_policy == fp_Explicit){
      printf("    %p  %8s  %10s  %10ld  %8ld  %-14p  %-14p  %s\n",
                p, ofs_str, size_str, size, size-2*TYPE_SIZE,
                status == ALLOC ? NULL : next, status == ALLOC ? NULL : prev,
                status == ALLOC ? "allocated" : "free");
    }
    
    free(ofs_str);
    free(size_str);

    void *fp = p + size - TYPE_SIZE;
    TYPE ftr = GET(fp);
    TYPE fsize = SIZE(ftr);
    TYPE fstatus = STATUS(ftr);

    if ((size != fsize) || (status != fstatus)) {
      errors++;
      printf("    --> ERROR: footer at %p with different properties: size: %lx, status: %lx\n", 
             fp, fsize, fstatus);
      //printf("appropriate size: %lx, status: %lx\n", size, status);
      //printf("header addr: %p, footer addr: %p\n", p, fp);
      mm_panic("mm_check");
    }

    p = p + size;
    if (size == 0) {
      printf("    WARNING: size 0 detected, aborting traversal.\n");
      break;
    }
  }

  printf("\n");
  if ((p == heap_end) && (errors == 0)) printf("  Block structure coherent.\n");
  printf("-------------------------------------------------------------------------------------------------\n");
}


