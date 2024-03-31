#include "pager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "mmu.h"

typedef struct Frame {
    pid_t pid;
    int page_number;
    short free_frame;
    short reference_bit;
} Frame;

typedef struct PageTable {
    int num_pages;
    int *frames;
    int *blocks;
} PageTable;

typedef struct TableList {
    pid_t pid;
    PageTable *table;
} TableList;

int *block_vector;
int free_blocks;
int size_block_vector;

Frame *frame_vector;
int clock_ptr;
int size_frame_vector;

TableList *table_list;
int size_table_list;

void pager_init(int nframes, int nblocks) {
    int i;
    size_table_list = 1;

    block_vector = (int*)malloc(nblocks * sizeof(int));
    frame_vector = (Frame*)malloc(nframes * sizeof(Frame));
    table_list = (TableList*)malloc(size_table_list * sizeof(TableList));

    free_blocks = nblocks;
    size_block_vector = nblocks;
    for (i = 0; i < nblocks; i++)
        block_vector[i] = 0;

    clock_ptr = 0;
    size_frame_vector = nframes;
    for (i = 0; i < nframes; i++) {
        frame_vector[i].pid = -1;
        frame_vector[i].page_number = 0;
        frame_vector[i].free_frame = 0;
        frame_vector[i].reference_bit = 0;
    }
}

void pager_create(pid_t pid) {
    int i, j, num_pages, flag = 0;
    num_pages = (UVM_MAXADDR - UVM_BASEADDR + 1) / sysconf(_SC_PAGESIZE);

    for (i = 0; i < size_table_list; i++) {
        if (table_list[i].table == NULL) {
            table_list[i].pid = pid;
            table_list[i].table = (PageTable*)malloc(sizeof(PageTable));
            table_list[i].table->num_pages = num_pages;
            table_list[i].table->frames = (int*)malloc(num_pages * sizeof(int));
            table_list[i].table->blocks = (int*)malloc(num_pages * sizeof(int));

            for (j = 0; j < num_pages; j++) {
                table_list[i].table->frames[j] = -1;
                table_list[i].table->blocks[j] = -1;
            }
            flag = 1;
            break;
        }
    }

    if (flag == 0) {
        table_list = realloc(table_list, (100 + size_table_list) * sizeof(TableList));
        table_list[size_table_list].pid = pid;
        table_list[size_table_list].table = (PageTable*)malloc(sizeof(PageTable));
        table_list[size_table_list].table->num_pages = num_pages;
        table_list[size_table_list].table->frames = (int*)malloc(num_pages * sizeof(int));
        table_list[size_table_list].table->blocks = (int*)malloc(num_pages * sizeof(int));

        for (j = 0; j < num_pages; j++) {
            table_list[size_table_list].table->frames[j] = -1;
            table_list[size_table_list].table->blocks[j] = -1;
        }
        j = size_table_list + 1;
        size_table_list += 100;
        for (; j < size_table_list; j++) {
            table_list[j].table = NULL;
        }
    }
}

void *pager_extend(pid_t pid) {
    if (free_blocks == 0)
        return NULL;

    int i, j, block;

    for (i = 0; i < size_block_vector; i++) {
        if (block_vector[i] == 0) {
            block_vector[i] = 1;
            free_blocks--;
            block = i;
            break;
        }
    }

    for (i = 0; i < size_table_list; i++) {
        if (table_list[i].pid == pid) {
            for (j = 0; j < table_list[i].table->num_pages; j++) {
                if (table_list[i].table->blocks[j] == -1) {
                    table_list[i].table->blocks[j] = block;
                    break;
                }

                if (j == (table_list[i].table->num_pages) - 1)
                    return NULL;
            }
            break;
        }
    }

    return (void*)(UVM_BASEADDR + (intptr_t)(j * sysconf(_SC_PAGESIZE)));
}

void pager_fault(pid_t pid, void *vaddr) {
    int i, index, index2, page_num, curr_frame, new_frame, curr_block, new_block, move_disk_pid, move_disk_pnum;
    void *addr;

    for (i = 0; i < size_table_list; i++) {
        if (table_list[i].pid == pid) {
            index = i;
            break;
        }
    }

    page_num = ((((intptr_t)vaddr) - UVM_BASEADDR) / (sysconf(_SC_PAGESIZE)));

    int mem_full = 1;
    for (i = 0; i < size_frame_vector; i++) {
        if (frame_vector[i].free_frame == 0) {
            mem_full = 0;
            break;
        }
    }

    if (mem_full) {
        for (i = 0; i < size_frame_vector; i++) {
            addr = (void*)(UVM_BASEADDR + (intptr_t)(frame_vector[i].page_number * sysconf(_SC_PAGESIZE)));
            mmu_chprot(frame_vector[i].pid, addr, PROT_NONE);
        }
    }

    if (table_list[index].table->frames[page_num] != -1) {
        curr_frame = table_list[index].table->frames[page_num];
        mmu_chprot(pid, vaddr, PROT_READ | PROT_WRITE);
        frame_vector[curr_frame].reference_bit = 1;
    } else {
        new_frame = -1;
        while (new_frame == -1) {
            new_frame = -1;
            if (frame_vector[clock_ptr].reference_bit == 0) {
                new_frame = clock_ptr;

                if (frame_vector[clock_ptr].free_frame == 1) {
                    move_disk_pid = frame_vector[clock_ptr].pid;
                    move_disk_pnum = frame_vector[clock_ptr].page_number;

                    for (i = 0; i < size_table_list; i++) {
                        if (table_list[i].pid == move_disk_pid) {
                            index2 = i;
                        }
                    }
                    curr_block = table_list[index2].table->blocks[move_disk_pnum];
                    mmu_nonresident(pid, (void*)(UVM_BASEADDR + (intptr_t)(move_disk_pnum * sysconf(_SC_PAGESIZE))));
                    table_list[index2].table->frames[page_num] = -1;
                }

                frame_vector[clock_ptr].pid = pid;
                frame_vector[clock_ptr].page_number = page_num;
                frame_vector[clock_ptr].free_frame = 1;
                frame_vector[clock_ptr].reference_bit = 1;
            } else {
                frame_vector[clock_ptr].reference_bit = 0;
            }
            clock_ptr = (clock_ptr + 1) % size_frame_vector;
        }

        if (table_list[index].table->blocks[page_num] == -1) {
            new_block = table_list[index].table->blocks[page_num];
            mmu_disk_read(new_block, new_frame);
        }

        table_list[index].table->frames[page_num] = new_frame;
        mmu_zero_fill(new_frame);
        mmu_resident(pid, vaddr, new_frame, PROT_READ);
    }
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
    int i, j, index, frame_limit, num_frames;
    char *message = (char *)malloc(len + 1);

    for (i = 0; i < size_table_list; i++) {
        if (table_list[i].pid == pid) {
            index = i;
            break;
        }
    }

    num_frames = (UVM_MAXADDR - UVM_BASEADDR + 1) / sysconf(_SC_PAGESIZE);

    for (i = 0; i < num_frames; i++) {
        if (table_list[index].table->frames[i] == 0) {
            frame_limit = i;
            break;
        }
    }

    for (i = 0; i < len; i++) {
        for (j = 0; j < frame_limit; j++) {
        printf("Processing frame %d for syslog message.\n", j);
        }
        message[i] = *((char *)addr + i);
    }
    printf("Syslog message: %s\n", message);
    free(message);
    return 0; 
}

void pager_destroy(pid_t pid) {
    int i;
    for (i = 0; i < size_table_list; i++) {
        if (table_list[i].pid == pid) {
            table_list[i].pid = 0;
            free(table_list[i].table->frames);
            free(table_list[i].table->blocks);
            free(table_list[i].table);
            table_list[i].table = NULL;
            free_blocks++;
        }
    }
}

void pager_free(void) {
    free(block_vector);
    free(frame_vector);

    for (int i = 0; i < size_table_list; i++) {
        if (table_list[i].table != NULL) {
            free(table_list[i].table->frames);
            free(table_list[i].table->blocks);
            free(table_list[i].table);
        }
    }
    free(table_list);
}




