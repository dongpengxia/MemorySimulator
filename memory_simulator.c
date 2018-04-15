//memory_simulator.c
//Dongpeng Xia
//This program simulates virtual memory management with a translation lookaside
//buffer (TLB with FIFO replacement policy) and a page table (demand paging).
//Statistics are computed regarding TLB hit rate and page fault rate.

//General Algorithm:
//read virtual addresses from file
//use virtual address to calculate page number and offset
//search for page number in TLB
//	if TLB hit
//		get frame number
//	else TLB miss
//		search for page number in page table
//			if page fault
//				load from disk
//				update page table
//		update TLB
//
//use frame number (from TLB) with page number and offset to output virtual
//address, physical address in memory, and byte at physical address

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FRAME_SIZE_BYTES 256	//size of one frame in bytes
#define NUM_FRAMES 256		//number of frames in memory
#define NUM_TLB_ENTRIES 16	//number of entries in TLB
#define OFFSET_MASK 0xFF	//mask for virtual address to compute offset
#define PAGE_NUM_MASK 0xFF00	//mask for virtual address to compute page num
#define PAGE_NUM_SHIFT 8	//shift for virtual address to compute page num
#define PAGE_SIZE_BYTES 256	//size of one page
#define VIRT_ADDR_CHAR_LENGTH 10 //2^32 = 4294967296 (10 digits max for 32 bits)

//store memory as 2D array
struct memory {
	char memory[NUM_FRAMES][FRAME_SIZE_BYTES];
	int highest_open_frame; //starts at 0
};

//store translation lookaside buffer as two parallel arrays for page numbers
//and frame numbers
struct tlb {
	int page_numbers[NUM_TLB_ENTRIES];
	int frame_numbers[NUM_TLB_ENTRIES];
	int num_filled;
	int head_position; //head for FIFO policy queue (oldest entry replaced
			   //when out of space)
};

//Singleton objects:
int page_table[PAGE_SIZE_BYTES];//page table
struct memory local_memory;	//memory
struct tlb TLB;			//translation lookaside buffer

//checks page table to see if there is an entry for page number, if not
//then loads from disk file
//returns number of page_faults
int check_page_table(int page_number, int offset, int disk_file);

//returns offset from virtual address
int get_offset(long virtual_address);

//returns page number from virtual address
int get_page_number(long virtual_address);

//initializes memory, sets starting frame to 0
void initialize_memory();

//initializes page table, all entries to -1
void initialize_page_table();

//initializes empty translation lookaside buffer
void initialize_TLB();

//loads from disk file to memory, returns frame number
int load_page_disk_to_memory(int page_number, int offset, int disk_file);

//returns frame number if page number in TLB, -1 if missing entry
int search_TLB(int page_number);

//updates page table to include <page_number, frame_number>
void update_page_table(int page_number, int frame_number);

//update TLB using FIFO policy to include <page_number, frame_number>
void update_TLB(int page_number, int frame_number);

//main
int main(int argc, const char * argv[])
{
	//check number of arguments
	if(argc != 2) {
		fprintf(stderr, "Error: Invalid number of arguments\n");
		exit(1);
	}
	
	//used for statistics
	int num_TLB_hits = 0;
	int num_Pg_faults = 0;
	int num_addresses_processed = 0;
	
	//start virtual memory system
	initialize_TLB();
	initialize_page_table();
	initialize_memory();
	
	//open file (hard disk/secondary storage)
	int disk_file = open("disk.bin",
			     O_RDWR | O_CREAT, S_IRWXO | S_IRWXU | S_IRWXG);
	
	//if hard disk file failed to open
	if(disk_file == -1)
		fprintf(stderr, "Error: %s\n", strerror(errno));
	
	//open file with virtual addresses to process
	FILE* virtual_address_file = fopen(argv[1], "r");
	
	//if addresses file failed to open
	if(virtual_address_file == NULL)
		fprintf(stderr, "Error: %s\n", strerror(errno));
	
	//sequentially process addresses in virtual address file
	char tmp[VIRT_ADDR_CHAR_LENGTH];
	while(fgets(tmp, VIRT_ADDR_CHAR_LENGTH, virtual_address_file) != NULL) {
		num_addresses_processed++;
		
		//calculate virtual address, page number, and offset
		long virtual_address = atol(tmp); //(long is at least 32 bits)
		int page_number = get_page_number(virtual_address);
		int offset = get_offset(virtual_address);
		
		//check TLB first
		int frame_number = search_TLB(page_number);
		if(frame_number == -1) {
			//TLB miss, check page_table
			num_Pg_faults = check_page_table(page_number, offset,
							 disk_file);
			frame_number = search_TLB(page_number);
		} else
			num_TLB_hits++; //TLB hit
		
		int physical_address = frame_number * FRAME_SIZE_BYTES + offset;
		int value = local_memory.memory[frame_number][offset];
		
		printf("Virtual address: %ld Physical address: %d Value: %d\n",
		       virtual_address, physical_address, value);
	}
	
	//close addresses file
	if(fclose(virtual_address_file) == EOF)
		fprintf(stderr, "Error: %s\n", strerror(errno));
	
	//close disk file
	if(close(disk_file) == -1)
		fprintf(stderr, "Error: %s\n", strerror(errno));
	
	//output statistics
	printf("Page Fault Rate: %f, TLB Hit Rate: %f\n",
	       (double) num_Pg_faults / num_addresses_processed,
	       (double) num_TLB_hits / num_addresses_processed);
	
	return 0;
}

//checks page table to see if there is an entry for page number, if not
//then loads from disk_file, returns number of page faults
int check_page_table(int page_number, int offset, int disk_file)
{
	static int num_page_faults = 0; //statistics tracking
	
	int frame_number;
	if(page_table[page_number] == -1) {
		num_page_faults++; //page fault
		frame_number = load_page_disk_to_memory(page_number, offset,
							disk_file);
		update_page_table(page_number, frame_number);
	} else
		frame_number = page_table[page_number];
	
	update_TLB(page_number, frame_number);
	return num_page_faults;
}

//returns offset from virtual_address
int get_offset(long virtual_address)
{
	return virtual_address & OFFSET_MASK;
}

//returns page number from virtual_address
int get_page_number(long virtual_address)
{
	return (int)((virtual_address & (PAGE_NUM_MASK)) >> PAGE_NUM_SHIFT);
}

//initializes memory, sets starting frame to 0
void initialize_memory()
{
	local_memory.highest_open_frame = 0;
}

//initializes page table, all entries to -1
void initialize_page_table()
{
	for(int i = 0; i < PAGE_SIZE_BYTES; i++)
		page_table[i] = -1;
}

//initializes empty translation lookaside buffer
void initialize_TLB()
{
	TLB.num_filled = 0;
	TLB.head_position = 0;
}

//loads from disk file to memory, returns frame number
int load_page_disk_to_memory(int page_number, int offset, int disk_file)
{
	//page fault -> read from disk to memory -> update page table
	
	//used to hold page read in from file
	char tmp_page[PAGE_SIZE_BYTES]={0};
	
	//random access to correct position in binary disk file
	if(lseek(disk_file, page_number * PAGE_SIZE_BYTES, SEEK_SET) == -1)
		fprintf(stderr, "Error: %s\n", strerror(errno));
	
	//read page from binary file (hard disk) into tmp_page
	if(read(disk_file, tmp_page, (size_t)sizeof(tmp_page)) == -1)
		fprintf(stderr, "Error: %s\n", strerror(errno));
	
	//copy from tmp_page to memory
	for(int i = 0; i < PAGE_SIZE_BYTES; i++)
		local_memory.memory[local_memory.highest_open_frame][i] =
								tmp_page[i];
	
	int frame_number = local_memory.highest_open_frame;
	local_memory.highest_open_frame++;
	return frame_number;
}

//returns frame number if page number in TLB, -1 if missing entry
int search_TLB(int page_number)
{
	int frame_number = -1;
	for(int index = 0; index < TLB.num_filled; index++) {
		if(TLB.page_numbers[(TLB.head_position + index)%NUM_TLB_ENTRIES]
		   == page_number) {
			frame_number  = TLB.frame_numbers[(TLB.head_position +
							index)%NUM_TLB_ENTRIES];
			break;
		}
	}
	return frame_number;
}

//updates page table to include <page_number, frame_number>
void update_page_table(int page_number, int frame_number)
{
	page_table[page_number] = frame_number;
}

//update TLB using FIFO policy to include <page_number, frame_number>
void update_TLB(int page_number, int frame_number)
{
	if(TLB.num_filled == NUM_TLB_ENTRIES) {
		//TLB full, replace and move head (FIFO policy)
		TLB.page_numbers[TLB.head_position] = page_number;
		TLB.frame_numbers[TLB.head_position] = frame_number;
		TLB.head_position = (TLB.head_position + 1) % NUM_TLB_ENTRIES;
	} else {
		//add new entry to TLB
		TLB.page_numbers[TLB.num_filled] = page_number;
		TLB.frame_numbers[TLB.num_filled] = frame_number;
		TLB.num_filled++;
	}
}