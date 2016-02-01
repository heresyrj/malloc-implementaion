//
// CS252: MyMalloc Project
//
// The current implementation gets memory from the OS
// every time memory is requested and never frees memory.
//
// You will implement the allocator as indicated in the handout.
//
// Also you will need to add the necessary locking mechanisms to
// support multi-threaded programs.
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include "MyMalloc.h"

static pthread_mutex_t mutex;

const int ArenaSize = 2097152;
const int NumberOfFreeLists = 1;

// Header of an object. Used both when the object is allocated and freed
typedef struct ObjectHeader
{
	size_t _objectSize;         // Real size of the object.
	int _allocated;             // 1 = yes, 0 = no 2 = sentinel
	struct ObjectHeader * _next;       // Points to the next object in the freelist (if free).
	struct ObjectHeader * _prev;       // Points to the previous object.
} ObjectHeader;

typedef struct ObjectFooter
{
	size_t _objectSize;
	int _allocated;
} ObjectFooter;

//STATE of the allocator

// Size of the heap
static size_t _heapSize;

// initial memory pool
static void * _memStart;

// number of chunks request from OS
static int _numChunks;

// True if heap has been initialized
// not initialized until allocateobject is called
static int _initialized = 0;

// Verbose mode
static int _verbose;

// # malloc calls
static int _mallocCalls;

// # free calls
static int _freeCalls;

// # realloc calls
static int _reallocCalls;

// # realloc calls
static int _callocCalls;

// Free list is a sentinel
static struct ObjectHeader _freeListSentinel; // Sentinel is used to simplify list operations
static struct ObjectHeader *_freeList;


//FUNCTIONS

//Initializes the heap
void initialize();

// Allocates an object
void * allocateObject( size_t size );

// Frees an object
void freeObject( void * ptr );

// Returns the size of an object
size_t objectSize( void * ptr );

// At exit handler
void atExitHandler();

//Prints the heap size and other information about the allocator
void print();
void print_list();

// Gets memory from the OS
void * getMemoryFromOS( size_t size );

void increaseMallocCalls() { _mallocCalls++; }

void increaseReallocCalls() { _reallocCalls++; }

void increaseCallocCalls() { _callocCalls++; }

void increaseFreeCalls() { _freeCalls++; }

extern void atExitHandlerInC()
{
	atExitHandler();
}

void initialize()
{
	// Environment var VERBOSE prints stats at end and turns on debugging
	// Default is on
	_verbose = 1;
	const char * envverbose = getenv( "MALLOCVERBOSE" );
	if ( envverbose && !strcmp( envverbose, "NO") ) {
		_verbose = 0;
	}

	pthread_mutex_init(&mutex, NULL);

	//_mem gives the ADDRESS of the memory chunk given by OS.
	void * _mem = getMemoryFromOS( ArenaSize + (2*sizeof(struct ObjectHeader)) + (2*sizeof(struct ObjectFooter)) );

	// In verbose mode register also printing statistics at exit
	atexit( atExitHandlerInC );

	/*
	the layout of the memory is like:

	sentinel
	---------
	|header	|
	|type	|
	---------

	|
	| linked
	|
	-----------------
					|
					|
	-------------------------------------------------------------------------------------
	|   footer     ||    header   |   usable memoryspace  |   footer   ||      header   |
	|(fencepost1)  ||  	      	  |						  |			   ||  (fencepost2) |
	-------------------------------------------------------------------------------------
	^				^									  ^				^
	|				|									  |		        |
	step1         step3                                 step4          step2

	------------------------>-------->------->------>--------->-------------------------
	                    			address decreases

	i.e  step2 = step1 - sizeof(fencepost1)

	NOTE: memory addresses increase from stack -> text
	stack has lower address,
	text has higher address,
	stack grows towards text: address increases
	heap grows towars stack: address decreases
	*/

	//establish fence posts
	//step1:fencepost1
	struct ObjectFooter * fencepost1 = (ObjectFooter *)_mem;
	fencepost1->_allocated = 1;
	fencepost1->_objectSize = 123456789;//the size doesn't matter


	char * temp = (char *)_mem + (2*sizeof(ObjectFooter)) + sizeof(ObjectHeader) + ArenaSize;

	//step2
	ObjectHeader * fencepost2 = (ObjectHeader *)temp;

	fencepost2->_allocated = 1;
	fencepost2->_objectSize = 123456789;
	fencepost2->_next = NULL;
	fencepost2->_prev = NULL;

	//initialize the list to point to the _mem
	//step3
	temp = (char *) _mem + sizeof(ObjectFooter);
	struct ObjectHeader * currentHeader = (struct ObjectHeader *) temp;

	//step4
	temp = (char *)_mem + sizeof(ObjectFooter) + sizeof(struct ObjectHeader) + ArenaSize;
	ObjectFooter * currentFooter = (struct ObjectFooter *) temp;

	//step5
	//(*header) = &(header)
	_freeList = &_freeListSentinel;

	//step6 initialize the first header node
	currentHeader->_objectSize = ArenaSize + sizeof(ObjectHeader) + sizeof(ObjectFooter); //initially around 2MB
	currentHeader->_allocated = 0;//all as free memory, only 1 chunk
	currentHeader->_next = _freeList;
	currentHeader->_prev = _freeList;
	currentFooter->_allocated = 0;
	currentFooter->_objectSize = currentHeader->_objectSize;

	//step7 handle the sentinel node
	_freeList->_prev = currentHeader;
	_freeList->_next = currentHeader;
	_freeList->_allocated = 2; // sentinel. no coalescing.
	_freeList->_objectSize = 0;

	//step8 get the start point of usable memory as _memStart
	_memStart = (char*) currentHeader;
}



/******************************************************************************************************************/
/**********************************************SELF DEFINED FUNCTIONS**********************************************/
/******************************************************************************************************************/
void * getNewMemChunkFromOS_and_Initialize() {
	//_mem gives the ADDRESS of the memory chunk given by OS.
	void * _mem = getMemoryFromOS( ArenaSize + (2*sizeof(ObjectHeader)) + (2*sizeof(ObjectFooter)) );

	//establish fence posts
	//step1:fencepost1
	struct ObjectFooter * fencepost1 = (struct ObjectFooter *)_mem;
	fencepost1->_allocated = 1;
	fencepost1->_objectSize = 123456789;//the size doesn't matter


	char * temp = (char *)_mem + (2*sizeof(struct ObjectFooter)) + sizeof(struct ObjectHeader) + ArenaSize;

	//step2
	struct ObjectHeader * fencepost2 = (struct ObjectHeader *)temp;

	fencepost2->_allocated = 1;
	fencepost2->_objectSize = 123456789;
	fencepost2->_next = NULL;
	fencepost2->_prev = NULL;

	//initialize the list to point to the _mem
	//step3
	temp = (char *) _mem + sizeof(struct ObjectFooter);
	ObjectHeader * currentHeader = (struct ObjectHeader *) temp;

	//step4
	temp = (char *)_mem + sizeof(struct ObjectFooter) + sizeof(struct ObjectHeader) + ArenaSize;
	ObjectFooter * currentFooter = (struct ObjectFooter *) temp;


	//set the size
	currentHeader->_objectSize = ArenaSize + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter); //initially around 2MB
	currentHeader->_allocated = 0;//all as free memory, only 1 chunk

	currentFooter->_allocated = 0;
	currentFooter->_objectSize = currentHeader->_objectSize;


	//add to the exiting freelist
	ObjectHeader * lastNode =  _freeList->_prev;

	lastNode->_next = currentHeader;
	currentHeader->_prev = lastNode;
	currentHeader->_next = _freeList;
	_freeList->_prev = currentHeader;

	//increase count of mem chunks that got from OS
	_numChunks++;

	return currentHeader;
}


//slef defined funcntion to createFormattedMemChunk
//it takes ptr of starting mem address
//&& size of the entire chunk of mem
//return pointer to usable memory
void * createFormattedMemChunk(char * start, size_t size, int allocated)
{
	//for header
	ObjectHeader * header = (ObjectHeader *)start;
	header->_objectSize = size;
	header->_allocated = allocated;
	//_next & _prev are not specified, should be done outside this function
	//since this fucntion does not differentiate Free Chunk from Used Chunk

	//for footer
	ObjectFooter * footer = (ObjectFooter *)(start + size - sizeof(ObjectFooter));
	footer->_objectSize = size;
	footer->_allocated = allocated;


	//for usable memory
	void * usableMem = (void *)(start + sizeof(ObjectHeader));

	//ignore the return if for free chunk
	return usableMem;
}
//IMPORTANT: freelist only contains headers

//when a free mem is allocated
void removeFromFreeList(ObjectHeader * current)
{
	ObjectHeader * prevNode = current->_prev;
	ObjectHeader * nextNode = current->_next;

	prevNode->_next = nextNode;
	nextNode->_prev = prevNode;
}

//when a free mem is allocated and the remainder free space is big enough
void removeFromFreeList_And_addRemainderToFreeList(ObjectHeader * current, ObjectHeader * remainder)
{
	ObjectHeader * prevNode = current->_prev;
	ObjectHeader * nextNode = current->_next;

	//added at the same position of node that to be removed
	prevNode->_next = remainder;
	remainder->_prev = prevNode;
	remainder->_next = nextNode;
	nextNode->_prev = remainder;

}


//always add immediately after setinel, so it's the first item in the list
void addToFreeList(ObjectHeader * current)
{
	current->_allocated = 0;
	ObjectFooter * currentFooter = (ObjectFooter *)((char *)current + current->_objectSize - sizeof(ObjectFooter));
	currentFooter->_allocated = 0;

	ObjectHeader * needle1 = _freeList;
	ObjectHeader * needle2 = needle1->_next;
	while(1)
	{
		//the node is between two normal nodes
		//DON'T MISS '=' after '>'
		int boolean1 = ((char *)current >= (char *)needle1) && ((char *)current < (char *)needle2);
		//the node is between last node and sentinel
		int boolean2 = ((char *)needle2 == (char *)_freeList) && ((char *)current > (char *)needle1);

		if ( boolean1 || boolean2 )
		{
			needle1->_next = current;
			needle2->_prev = current;
			current->_next = needle2;
			current->_prev = needle1;
			break;
		}

		needle1 = needle1->_next;
		needle2 = needle2->_next;
	}

}


//this function takes 2 pointers as arguments
//it will merge them into one and add the merged to the freelist
//NOTE: the order matters. left pointer has to be pointer of header of left mem block
ObjectHeader * mergeMems(ObjectHeader * leftHeader, ObjectFooter * rightFooter) 
{
	size_t mergedSize = leftHeader->_objectSize + rightFooter->_objectSize;
	

	//determine which side is the free one
	if (leftHeader->_allocated == 0) 
	{
		//remove the curren free block from list
		removeFromFreeList(leftHeader);
		
	}
	if (rightFooter->_allocated == 0) 
	{
		//remove the curren free block from list
		ObjectHeader * rightHeader = (ObjectHeader *)( (char *)rightFooter + sizeof(ObjectFooter) - rightFooter->_objectSize);
		removeFromFreeList(rightHeader);
	}

	//then change the size. 
	//IMPORTANT: if change the size first, more will be freed then what's supposed to be
		leftHeader->_objectSize = mergedSize;
		rightFooter->_objectSize = mergedSize;

	return leftHeader;
}


/********************************************************END*******************************************************/
/******************************************************************************************************************/



//when find a chunk of memory that's big enough
//to tell if needed to split and make the remainder a node in freelist
//the remainder has to be >= sizeof(header + footer) + 16 = 64 bytes
void * allocateObject( size_t size )
{	
	//step1: check if mem is initialized
	if ( !_initialized ) {
		_initialized = 1;
		initialize();
	}

	// step 2: get the actual size needed
	// Add the ObjectHeader/Footer to the size and round the total size up to a multiple of
	// 8 bytes for alignment.
	size_t roundedSize = (size + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter) + 7) & ~7;


	// step3: traverse the freelist to find the first node that's large engough
	ObjectHeader * needle = _freeList->_next;//needle points to header of the first node in the list

	int minSize = sizeof(ObjectHeader) + sizeof(ObjectFooter) + 8;

	void * ptr_2b_returned = NULL;


	//all nodes in list are free, don't need to check
	while( needle->_allocated != 2) 
	{//when it's 2, means reach the sentinel node


		size_t size_dif = (needle->_objectSize) - roundedSize;

		//step4: if the first fit found.
		if ( size_dif >= 0 ) 
		{
			//step5: determine if the remainder is big enough
			//if so, split; if not use entire chunk as 1 node
			if (size_dif <= minSize) //one node
			{
				//the node is for use, thus has to return mem ptr
				ptr_2b_returned = (void *)createFormattedMemChunk((char *)needle, needle->_objectSize, 1);
								//take out "Header of this node" out of the freelist
				removeFromFreeList(needle);

			}
			else //split, one for use, which taken out from list; one to be added into list
			{
				char * ptr_remainder = (char *)needle + roundedSize;//gives the starting point of remainder node

				//createFormattedMemChunk(char * ptr, size_t size) -> for node that'll be used 
				ptr_2b_returned = createFormattedMemChunk((char *)needle, roundedSize, 1);

				//createFormattedMemChunk(char * ptr, size_t size) -> for remainder node
				createFormattedMemChunk(ptr_remainder, size_dif, 0);

				//remove used and add remainder to the freelist
				//removeFromFreeList_And_addRemainderToFreeList((ObjectHeader *)needle, (ObjectHeader *)ptr_remainder);
				removeFromFreeList((ObjectHeader *)needle);
				addToFreeList((ObjectHeader *)ptr_remainder);
			}
		}

		needle = needle->_next;
	}

	if (ptr_2b_returned == NULL) //meaning no available mem for the request
	{
		//get another 2MB from OS and format it
		//needle points to the header(real one not fencepost) of the new chunk
		ObjectHeader * firstHeaderInNewChunk = getNewMemChunkFromOS_and_Initialize();
		char * ptr_remainder = (char *)firstHeaderInNewChunk + roundedSize;//header of the free chunk

		//split it and add the remainder to freeelist.
		//size needed is roundedSize
		ptr_2b_returned = createFormattedMemChunk((char *)firstHeaderInNewChunk, roundedSize, 1);//for use

        size_t size_dif = firstHeaderInNewChunk->_objectSize - roundedSize;

		ptr_remainder = createFormattedMemChunk(ptr_remainder, size_dif, 0);//remainder to be added to list

		//freelist handling
		//removeFromFreeList_And_addRemainderToFreeList((ObjectHeader *)needle, (ObjectHeader *)ptr_remainder);
		removeFromFreeList(firstHeaderInNewChunk);
		addToFreeList((ObjectHeader *)ptr_remainder);
	}


	//unlock
	pthread_mutex_unlock(&mutex);
	// Return a pointer to usable memory
	return ptr_2b_returned;

}


void freeObject( void * ptr )
{
	//ptr given is pointing to the usable mem
	

	//check the attributes of current block, get header and footer
	ObjectHeader * header = (ObjectHeader *)( (char *)ptr - sizeof(ObjectHeader) );
	int currentSize = header->_objectSize;//real size including header and footer
	ObjectFooter * footer = (ObjectFooter *)((char *)header + currentSize - sizeof(ObjectFooter));

	//check left side, get its header and footer
	ObjectFooter * footer_of_leftBlock = (ObjectFooter *) ( (char *)header - sizeof(ObjectFooter) );
	size_t leftBlockSize = footer_of_leftBlock->_objectSize;
	ObjectHeader * header_of_leftBlock = (ObjectHeader *)( (char *)footer_of_leftBlock + sizeof(ObjectFooter) - leftBlockSize );
	int leftAllocated = footer_of_leftBlock->_allocated;


	//check right side, get its header and footer
	ObjectHeader * header_of_rightBlock = (ObjectHeader *) ( (char *)header + currentSize );
	size_t rightBlockSize = header_of_rightBlock->_objectSize;
	ObjectFooter * footer_of_rightBlock = (ObjectFooter *)( (char *)header_of_rightBlock + rightBlockSize - sizeof(ObjectFooter) );
	int rightAllocated = header_of_rightBlock->_allocated;

	//try to free mem depending on the case
	//printf("leftAllocated == %d && rightAllocated == %d\n", leftAllocated, rightAllocated);

	ObjectHeader * ptr_2_add;
	
	if(leftAllocated == 0 && rightAllocated == 0) 
	{
		//current block will be gone and merged with left block
		//printf("left is %d, right is %d\n", leftAllocated, rightAllocated);
		ObjectHeader * temp;
		temp = mergeMems(header_of_leftBlock, footer);//temp now is left header
		ptr_2_add = mergeMems(temp, footer_of_rightBlock);

	} 
	if(leftAllocated == 0 && rightAllocated == 1) 
	{
		//right block will be gone and merge with already merged left two blocks;
		//printf("left is %d, right is %d\n", leftAllocated, rightAllocated);
        ptr_2_add = mergeMems(header_of_leftBlock, footer);
        
	}
	if(leftAllocated == 1 && rightAllocated == 0) 
	{
		//right block will be gone and merge with already merged left two blocks;
		//printf("left is %d, right is %d\n", leftAllocated, rightAllocated);
        ptr_2_add = mergeMems(header, footer_of_rightBlock);
	}
	if (leftAllocated == 1 && rightAllocated == 1) 
	{
		//printf("left is %d, right is %d\n", leftAllocated, rightAllocated);
		ptr_2_add = header;
	}
	
		
	addToFreeList(ptr_2_add);

}

size_t objectSize( void * ptr )
{
// Return the size of the object pointed by ptr. We assume that ptr is a valid obejct.
	struct ObjectHeader * o = (struct ObjectHeader *) ( (char *) ptr - sizeof(struct ObjectHeader) );

// Substract the size of the header
	return o->_objectSize;
}

void print()
{
	printf("\n-------------------\n");

	printf("HeapSize:\t%zd bytes\n", _heapSize );
	printf("# mallocs:\t%d\n", _mallocCalls );
	printf("# reallocs:\t%d\n", _reallocCalls );
	printf("# callocs:\t%d\n", _callocCalls );
	printf("# frees:\t%d\n", _freeCalls );

	printf("\n-------------------\n");
}

void print_list()
{
	printf("FreeList: ");
	if ( !_initialized ) {
		_initialized = 1;
		initialize();
	}
	struct ObjectHeader * ptr = _freeList->_next;
	while(ptr != _freeList){
		// ptr is the header of first header in the list
		// _memstart is the header of the just initialized mem chunk 
		// which is supposed to be used when the first malloc is called
		long offset = (long)ptr - (long)_memStart;
		printf("[offset:%ld,size:%zd]",offset,ptr->_objectSize);
		ptr = ptr->_next;
		if(ptr != NULL){
			printf("->");
		}
	}
	printf("\n");
}

void * getMemoryFromOS( size_t size )
{
// Use sbrk() to get memory from OS
	_heapSize += size;

	void * _mem = sbrk( size );

	if(!_initialized){
		_memStart = _mem;
	}

	_numChunks++;

	return _mem;
}

void atExitHandler()
{
// Print statistics when exit
	if ( _verbose ) {
		print();
	}
}

//
// C interface
//

extern void *
malloc(size_t size)
{
	pthread_mutex_lock(&mutex);
	increaseMallocCalls();

	return allocateObject( size );
}

extern void
free(void *ptr)
{
	pthread_mutex_lock(&mutex);
	increaseFreeCalls();

	if ( ptr == 0 ) {
		// No object to free
		pthread_mutex_unlock(&mutex);
		return;
	}

	freeObject( ptr );
}

extern void *
realloc(void *ptr, size_t size)
{
	pthread_mutex_lock(&mutex);
	increaseReallocCalls();

// Allocate new object
	void * newptr = allocateObject( size );

// Copy old object only if ptr != 0
	if ( ptr != 0 ) {

// copy only the minimum number of bytes
		size_t sizeToCopy =  objectSize( ptr );
		if ( sizeToCopy > size ) {
			sizeToCopy = size;
		}

		memcpy( newptr, ptr, sizeToCopy );

// Free old object
		freeObject( ptr );
	}

	return newptr;
}

extern void *
calloc(size_t nelem, size_t elsize)
{
	pthread_mutex_lock(&mutex);
	increaseCallocCalls();

// calloc allocates and initializes
	size_t size = nelem * elsize;

	void * ptr = allocateObject( size );

	if ( ptr ) {
// No error
// Initialize chunk with 0s
		memset( ptr, 0, size );
	}

	return ptr;
}

