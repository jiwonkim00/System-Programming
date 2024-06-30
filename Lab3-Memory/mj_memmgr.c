
//--------------------------------------------------------------------------------------------------
// System Programming                       Memory Lab                                    Fall 2023
//
/// @file
/// @brief dynamic memory manager
/// @author Lauren Minjung Kwon
/// @studid 2020-10461
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
// - minimal block size: 32 bytes (header +footer + 2 data words)
// - h,f: header/footer of free block
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
// - allocation policies: first, next, best fit
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
static void *next_block    = NULL;                     ///< next block used by next-fit policy
static size_t CHUNKSIZE    = 1<<16;                    ///< minimal data segment allocation unit (adjust to tune performance)
static size_t SHRINKTHLD   = 1<<16;                    ///< threshold to shrink heap (implementation optional; adjust to tune performance)
static int  mm_initialized = 0;                        ///< initialized flag (yes: 1, otherwise 0)
static int  mm_loglevel    = 0;                        ///< log level (0: off; 1: info; 2: verbose)

/// @}

/// @name Macro definitions
/// @{
#define MAX(a, b)          ((a) > (b) ? (a) : (b))     ///< MAX function

#define TYPE               unsigned long               ///< word type of heap
#define TYPE_SIZE          sizeof(TYPE)                ///< size of word type

#define ALLOC              1                           ///< block allocated flag
#define FREE               0                           ///< block free flag
#define STATUS_MASK        ((TYPE)(0x7))               ///< mask to retrieve flagsfrom header/footer
#define SIZE_MASK          (~STATUS_MASK)              ///< mask to retrieve size from header/footer

#define BS                 32                          ///< minimal block size. Must be a power of 2
#define BS_MASK            (~(BS-1))                   ///< alignment mask

#define WORD(p)            ((TYPE)(p))                 ///< convert pointer to TYPE
#define PTR(w)             ((void*)(w))                ///< convert TYPE to void*

#define PREV_PTR(p)        ((p)-TYPE_SIZE)             ///< get pointer to word preceeding p
#define NEXT_PTR(p)        ((p)+TYPE_SIZE)             ///< get pointer to word succeeding p
#define HDR2FTR(p)         ((p)+GET_SIZE(p)-TYPE_SIZE) ///< get footer for given header
#define FTR2HDR(p)         ((p)-GET_SIZE(p)+TYPE_SIZE) ///< get footer for given header
#define HDR2NEXTHDR(p)     ((p)+GET_SIZE(p))           ///< get next block's header from header p

#define PACK(size,status)  ((size) | (status))         ///< pack size & status into boundary tag
#define SIZE(v)            (v & SIZE_MASK)             ///< extract size from boundary tag
#define STATUS(v)          (v & STATUS_MASK)           ///< extract status from boundary tag

#define PUT(p, v)          (*(TYPE*)(p) = (TYPE)(v))   ///< write word v to *p
#define GET(p)             (*(TYPE*)(p))               ///< read word at *p
#define GET_SIZE(p)        (SIZE(GET(p)))              ///< extract size from header/footer
#define GET_STATUS(p)      (STATUS(GET(p)))            ///< extract status from header/footer

#define DEBUG 1

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


static void* ff_get_free_block(size_t);
static void* nf_get_free_block(size_t);
static void* bf_get_free_block(size_t);

/// @brief ititialize memmgr. set allocation policy, increment heap size by CHUNKSIZE because initial size is zero, set pointers to appropriate values, set sentinel blocks and header, footer of the blank heap(one block)
/// @param ap allocation policy
void mm_init(AllocationPolicy ap)
{
  LOG(1, "mm_init()");

  //
  // set allocation policy
  //
  char *apstr;
  switch (ap) {
    case ap_FirstFit: get_free_block = ff_get_free_block; apstr = "first fit"; break;
    case ap_NextFit:  get_free_block = nf_get_free_block; apstr = "next fit";  break;
    case ap_BestFit:  get_free_block = bf_get_free_block; apstr = "best fit";  break;
    default: PANIC("Invalid allocation policy.");
  }
  LOG(2, "  allocation policy       %s\n", apstr);

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

  //initial heap size=0, so have to sbrk
  if(ds_sbrk(CHUNKSIZE) != ds_heap_brk) PANIC("Something wrong when ds_sbrk");
  
  //initialize pointers
  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
  heap_start = ds_heap_start + BS;
  heap_end = ds_heap_brk - BS;
  next_block = heap_start;
  PUT(PREV_PTR(heap_start), 0x1);//initial sentinel
  PUT(heap_end, 0x1);//end sentinel
  size_t size = CHUNKSIZE - 64;
  PUT(heap_start, PACK(size, FREE));
  PUT(PREV_PTR(heap_end), PACK(size, FREE));
  // heap is initialize

  mm_initialized = 1;
}

/// @brief returns a pointer to an allocated payload block of at least size bytes. 32 bytes aligned, add header and footer
/// @param size size of data only (excluding header, footer)
/// @retval pointer to payload block if success, NULL if fail
void* mm_malloc(size_t size)
{
  LOG(1, "mm_malloc(0x%lx)", size);

  assert(mm_initialized);

  size_t newsize = (((size + 2*TYPE_SIZE - 1) >> 5) + 1) << 5; //round up (size+16) to 32
  void* blk = get_free_block(newsize);
  if(blk == NULL) return NULL;
  void* old_ftr = HDR2FTR(blk);
  size_t oldsize = GET_SIZE(blk);

  PUT(blk, PACK(newsize, ALLOC)); //new header of allocated blk
  void* new_ftr = HDR2FTR(blk);
  PUT(new_ftr, PACK(newsize, ALLOC)); //new footer of allocated blk
  LOG(2, "  allocated block at %p, size %lu", blk, GET_SIZE(blk));

  if((oldsize - newsize) >= 32){ //split block
    PUT(NEXT_PTR(new_ftr), PACK((oldsize - newsize), FREE)); //new header of free blk
    PUT(old_ftr, PACK((oldsize - newsize), FREE)); //new footer of free blk
    LOG(2, "  splited free block at %p, size %lu\n", NEXT_PTR(new_ftr), oldsize - newsize);
  } else if((oldsize - newsize) != 0){
    PANIC("not 32 byte aligned");
  }

  return NEXT_PTR(blk);
}

/// @breif returns a pointer to an allocated payload of at least nmemb*size bytes, intitialize to zero
/// @param nmemb number of members
/// @param size size of each member
void* mm_calloc(size_t nmemb, size_t size)
{
  LOG(1, "mm_calloc(0x%lx, 0x%lx)", nmemb, size);

  assert(mm_initialized);

  //
  // calloc is simply malloc() followed by memset()
  //
  void *payload = mm_malloc(nmemb * size);

  if (payload != NULL) memset(payload, 0, nmemb * size);

  return payload;
}

/// @brief extend ptr to size if possible, if not, allocate new block of size and copy values
/// @retval pointer to start of payload if success, NULL if fail
void* mm_realloc(void *ptr, size_t size)
{
  LOG(1, "mm_realloc(%p, 0x%lx)", ptr, size);

  if(ptr == NULL) {
    LOG(2, "  ptr is null, perform malloc");
    return mm_malloc(size);
  } else if(size == 0){
    LOG(2, "  size is 0, perform free");
    mm_free(ptr);
    return NULL;
  }

  assert(mm_initialized);
  void* cur_hdr = PREV_PTR(ptr);
  void* cur_ftr;
  void* next_hdr = HDR2NEXTHDR(cur_hdr);
  size_t cursize = GET_SIZE(cur_hdr);
  size_t nextsize = GET_SIZE(next_hdr);
  size_t newsize = (((size + 2*TYPE_SIZE - 1) >> 5) + 1) << 5; //round up (size+16) to 32

  if(newsize <= cursize){
    LOG(2, "case1");
    //shrinking size
    PUT(cur_hdr, PACK(newsize, ALLOC));
    cur_ftr = HDR2FTR(cur_hdr);
    PUT(cur_ftr, PACK(newsize, ALLOC));
    if((cursize - newsize) >= 32){//split block and coalesce
      void* free_hdr = NEXT_PTR(cur_ftr);
      size_t freesize = cursize - newsize;
      if(GET_STATUS(next_hdr) == FREE){//coalesce
        freesize += GET_SIZE(next_hdr);
      }
      PUT(free_hdr, PACK(freesize, FREE));
      PUT(HDR2FTR(free_hdr), PACK(freesize, FREE));
    }
    return ptr;
  } else if((GET_STATUS(next_hdr) == FREE) && ((cursize + nextsize) >= newsize)){
    LOG(2, "case2");
    //next block free & sufficiently big
    size_t totsize = GET_SIZE(cur_hdr) + GET_SIZE(next_hdr);
    PUT(cur_hdr, PACK(newsize, ALLOC));
    cur_ftr = HDR2FTR(cur_hdr);
    PUT(cur_ftr, PACK(newsize, ALLOC));
    if((totsize - newsize) >= 32){//split block
      PUT(NEXT_PTR(cur_ftr), PACK(totsize - newsize, FREE));
      PUT(HDR2FTR(NEXT_PTR(cur_ftr)), PACK(totsize - newsize, FREE));
    }
    return ptr;
  } else{
    LOG(2, "case3");
    //need to move
    void* payload = mm_malloc(size);
    if(payload == NULL) return NULL;
    memcpy(payload, ptr, GET_SIZE(cur_hdr) - 2*TYPE_SIZE);
    mm_free(ptr);
    return payload;
  }

}

/// @breif frees the block pointed to by ptr which is start of payload
/// @param ptr pointer to the start of payload
void mm_free(void *ptr)
{
  LOG(1, "mm_free(%p)", ptr);
  assert(mm_initialized);
  if(ptr == NULL){
    LOG(2, "  ptr is null");
    return;
  }

  ptr = PREV_PTR(ptr);//header of current block
  void *prev_ftr = PREV_PTR(ptr);
  void *next_hdr = HDR2NEXTHDR(ptr);
  TYPE prev_status = GET_STATUS(prev_ftr);
  TYPE next_status = GET_STATUS(next_hdr);
  void *new_hdr;
  TYPE size = GET_SIZE(ptr);

  if((prev_status == ALLOC) && (next_status == ALLOC)){
    PUT(ptr, PACK(size, FREE));
    PUT(HDR2FTR(ptr), PACK(size, FREE));
    new_hdr = ptr;
  }else if((prev_status == ALLOC) && (next_status == FREE)){
    size += GET_SIZE(next_hdr);
    PUT(ptr, PACK(size, FREE));
    PUT(HDR2FTR(next_hdr), PACK(size, FREE));
    new_hdr = ptr;
  }else if((prev_status == FREE) && (next_status == ALLOC)){
    size += GET_SIZE(prev_ftr);
    PUT(FTR2HDR(prev_ftr), PACK(size, FREE));
    PUT(HDR2FTR(ptr), PACK(size, FREE));
    new_hdr = FTR2HDR(prev_ftr);
  }else{//FREE FREE
    size += GET_SIZE(prev_ftr) + GET_SIZE(next_hdr);
    PUT(FTR2HDR(prev_ftr), PACK(size, FREE));
    PUT(HDR2FTR(next_hdr), PACK(size, FREE));
    new_hdr = FTR2HDR(prev_ftr);
  }

  LOG(2, "  free block's header: %p, size: %lu\n", new_hdr, size);

}

/// @brief increases heap size by calling ds_sbrk and adjusting pointer values afterwards
/// @retval 0 on success, -1 on failure
static int mm_sbrk(){
  LOG(1, "mm_sbrk()");

  if(ds_sbrk(CHUNKSIZE) != ds_heap_brk){
    return -1;
  }

  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
  void* old_heap_end = heap_end;
  heap_start = ds_heap_start + BS;//optional
  heap_end = ds_heap_brk - BS;
  PUT(PREV_PTR(heap_start), 0x1);//initial sentinel, optional
  PUT(heap_end, 0x1);//end sentinel
  if((GET_STATUS(PREV_PTR(old_heap_end))) == FREE){
    //if old last block is free, need to coalesce
    size_t size = GET_SIZE(PREV_PTR(old_heap_end)) + CHUNKSIZE;
    PUT(FTR2HDR(PREV_PTR(old_heap_end)), PACK(size, FREE));//header of new free block
    PUT(PREV_PTR(heap_end), PACK(size, FREE));//footer of new free block
  }else{
    //if old last block is allocated
    PUT(old_heap_end, PACK(CHUNKSIZE, FREE));
    PUT(PREV_PTR(heap_end), PACK(CHUNKSIZE, FREE));
  }
  return 0;
}

/// @name block allocation policies
/// @{

/// @brief find and return a free block of at least @a size bytes (first fit)
/// @param size size of block (including header & footer tags), in bytes, always comes 32 bytes aligned
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
/// 
static void* ff_get_free_block(size_t size)
{
  LOG(1, "ff_get_free_block(1x%lx (%lu))", size, size);

  assert(mm_initialized);

  //
  // TODO
  void* ptr = heap_start;
  while(WORD(ptr) < WORD(heap_end)){
    if((GET_STATUS(ptr) == FREE) && (GET_SIZE(ptr) >= size)){
      return ptr;
    }
    ptr = HDR2NEXTHDR(ptr);
  }
  //reached heap_end and could not find block
  if(mm_sbrk() == -1) return NULL;

  //try again after sbrk
  return ff_get_free_block(size);
}


/// @brief find and return a free block of at least @a size bytes (next fit)
/// @param size size of block (including header & footer tags), in bytes, always comes 32 bytes aligned
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* nf_get_free_block(size_t size)
{
  LOG(1, "nf_get_free_block(0x%x (%lu))", size, size);

  assert(mm_initialized);

  // TODO
  
  void* ptr = next_block;
  //search until end of heap
  while(WORD(ptr) < WORD(heap_end)){
    if((GET_STATUS(ptr) == FREE) && (GET_SIZE(ptr) >= size)){
      next_block = ptr;
      return ptr;
    }
    ptr = HDR2NEXTHDR(ptr);
  }
  //search again from the start of heap
  ptr = heap_start;
  while(WORD(ptr) < WORD(next_block)){
    if((GET_STATUS(ptr) == FREE) && (GET_SIZE(ptr) >= size)){
      next_block = ptr;
      return ptr;
    }
    ptr = HDR2NEXTHDR(ptr);
  }
 
  //increase heap and try again
  if(mm_sbrk() == -1) return NULL;
  return nf_get_free_block(size);
}

/// @brief find and return a free block of at least @a size bytes (best fit)
/// @param size size of block (including header & footer tags), in bytes, always comes 32 bytes aligned
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* bf_get_free_block(size_t size)
{
  LOG(1, "bf_get_free_block(0x%lx (%lu))", size, size);

  assert(mm_initialized);

  //
  // TODO
  void* best_ptr = NULL;
  TYPE best_size = (TYPE) -1; //max value of TYPE
  void* ptr = heap_start;

  while(WORD(ptr) < WORD(heap_end)){
    TYPE ptr_size = GET_SIZE(ptr);
    if((GET_STATUS(ptr) == FREE) && (ptr_size >= size)){
      if(ptr_size == size){
        return ptr;
      } else if(ptr_size < best_size){
        best_ptr = ptr;
        best_size = ptr_size;
      }
    }
    ptr = HDR2NEXTHDR(ptr);
  }
  
  if(best_ptr != NULL){// could find appropriate block
    return best_ptr;
  } else{// couldn't find appropriate block
    if(mm_sbrk() == -1) return NULL;
    return bf_get_free_block(size);
  }
}

/// @}

void mm_setloglevel(int level)
{
  mm_loglevel = level;
}


void mm_check(void)
{
  assert(mm_initialized);

  void *p;
  char *apstr;
  if (get_free_block == ff_get_free_block) apstr = "first fit";
  else if (get_free_block == nf_get_free_block) apstr = "next fit";
  else if (get_free_block == bf_get_free_block) apstr = "best fit";
  else apstr = "invalid";

  printf("----------------------------------------- mm_check ----------------------------------------------\n");
  printf("  ds_heap_start:          %p\n", ds_heap_start);
  printf("  ds_heap_brk:            %p\n", ds_heap_brk);
  printf("  heap_start:             %p\n", heap_start);
  printf("  heap_end:               %p\n", heap_end);
  printf("  allocation policy:      %s\n", apstr);
  printf("  next_block:             %p\n", next_block);

  printf("\n");
  p = PREV_PTR(heap_start);
  printf("  initial sentinel:       %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  p = heap_end;
  printf("  end sentinel:           %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  printf("\n");
  printf("  blocks:\n");

  printf("    %-14s  %8s  %10s  %10s  %8s  %s\n", "address", "offset", "size (hex)", "size (dec)", "payload", "status");

  long errors = 0;
  p = heap_start;
  while (p < heap_end) {
    char *ofs_str, *size_str;

    TYPE hdr = GET(p);
    TYPE size = SIZE(hdr);
    TYPE status = STATUS(hdr);

    if (asprintf(&ofs_str, "0x%lx", p-heap_start) < 0) ofs_str = NULL;
    if (asprintf(&size_str, "0x%lx", size) < 0) size_str = NULL;
    printf("    %p  %8s  %10s  %10ld  %8ld  %s\n",
           p, ofs_str, size_str, size, size-2*TYPE_SIZE, status == ALLOC ? "allocated" : "free");

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
