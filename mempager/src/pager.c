#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mmu.h"
#include "pager.h"

/* Seção de inclusão da implementação da dlist do tp1 */

struct dlist {
	struct dnode *head;
	struct dnode *tail;
	int count;
};

struct dnode {
	struct dnode *prev;
	struct dnode *next;
	void *data;
};

typedef void (*dlist_data_func)(void *data);
typedef int (*dlist_cmp_func)(const void *e1, const void *e2, void *userdata); 

struct dlist *dlist_create(void);
void dlist_destroy(struct dlist *dl, dlist_data_func);

void *dlist_pop_left(struct dlist *dl);
void *dlist_pop_right(struct dlist *dl);
void *dlist_push_right(struct dlist *dl, void *data);

/* this function calls =cmp to compare =data and each value in =dl.  if a
 * match is found, it is removed from the list and its pointer is returned.
 * returns NULL if no match is found. */
void *dlist_find_remove(struct dlist *dl, void *data, dlist_cmp_func cmp,
			void *userdata);

int dlist_empty(struct dlist *dl);

/* gets the data at index =idx.  =idx can be negative. */
void * dlist_get_index(const struct dlist *dl, int idx);
/* changes the data at index =idx.  does nothing if =idx does not exist. */
void dlist_set_index(struct dlist *dl, int idx, void *data);

struct dlist *dlist_create(void) /* {{{ */
{
	struct dlist *dl = malloc(sizeof(struct dlist));
	assert(dl);
	dl->head = NULL;
	dl->tail = NULL;
	dl->count = 0;
	return dl;
} /* }}} */

void dlist_destroy(struct dlist *dl, dlist_data_func cb) /* {{{ */
{
	while(!dlist_empty(dl)) {
		void *data = dlist_pop_left(dl);
		if(cb) cb(data);
	}
	free(dl);
} /* }}} */

void *dlist_pop_left(struct dlist *dl) /* {{{ */
{
	void *data;
	struct dnode *node;

	if(dlist_empty(dl)) return NULL;

	node = dl->head;

	dl->head = node->next;
	if(dl->head == NULL) dl->tail = NULL;
	if(node->next) node->next->prev = NULL;

	data = node->data;
	free(node);

	dl->count--;
	assert(dl->count >= 0);
	return data;
} /* }}} */

void *dlist_pop_right(struct dlist *dl) /* {{{ */
{
	void *data;
	struct dnode *node;

	if(dlist_empty(dl)) return NULL;

	node = dl->tail;

	dl->tail = node->prev;
	if(dl->tail == NULL) dl->head = NULL;
	if(node->prev) node->prev->next = NULL;

	data = node->data;
	free(node);

	dl->count--;
	assert(dl->count >= 0);
	return data;
} /* }}} */

void *dlist_push_right(struct dlist *dl, void *data) /* {{{ */
{
	struct dnode *node = malloc(sizeof(struct dnode));
	assert(node);

	node->data = data;
	node->prev = dl->tail;
	node->next = NULL;

	if(dl->tail) dl->tail->next = node;
	dl->tail = node;

	if(dl->head == NULL) dl->head = node;

	dl->count++;
	return data;
} /* }}} */

void *dlist_find_remove(struct dlist *dl, void *data, /* {{{ */
		dlist_cmp_func cmp, void *user_data)
{
	struct dnode *curr;
	for(curr = dl->head; curr; curr = curr->next) {
		if(!curr->data) continue;
		if(cmp(curr->data, data, user_data)) continue;
		void *ptr = curr->data;
		if(dl->head == curr) dl->head = curr->next;
		if(dl->tail == curr) dl->tail = curr->prev;
		if(curr->prev) curr->prev->next = curr->next;
		if(curr->next) curr->next->prev = curr->prev;
		dl->count--;
		free(curr);
		return ptr;
	}
	return NULL;
} /* }}} */

int dlist_empty(struct dlist *dl) /* {{{ */
{
	if(dl->head == NULL) {
		assert(dl->tail == NULL);
		assert(dl->count == 0);
		return 1;
	} else {
		assert(dl->tail != NULL);
		assert(dl->count > 0);
		return 0;
	}
} /* }}} */

void * dlist_get_index(const struct dlist *dl, int idx) /* {{{ */
{
	struct dnode *curr;
	if(idx >= 0) {
		curr = dl->head;
		while(curr && idx--) curr = curr->next;
	} else {
		curr = dl->tail;
		while(curr && ++idx) curr = curr->prev;
	}
	if(!curr) return NULL;
	return curr->data;
} /* }}} */

void dlist_set_index(struct dlist *dl, int idx, void *data) /* {{{ */
{
	struct dnode *curr;
	if(idx >= 0) {
		curr = dl->head;
		while(curr && idx--) curr = curr->next;
	} else {
		curr = dl->tail;
		while(curr && ++idx) curr = curr->prev;
	}
	if(!curr) return;
	curr->data = data;
} /* }}} */


/* Seção da implementação do paginador propriamente dito */

typedef struct {
    int numeroBloco;
    int numeroQuadro;
    bool modificada;
    bool valida;
    intptr_t vaddr;
} Pagina;

typedef struct {
    struct dlist *paginas;
    pid_t pid;
} TabelaPaginas;

typedef struct {
    Pagina *pagina;
    bool utilizado; 
} Bloco;

typedef struct {
    Bloco *blocos;
    int numeroBlocos;
} TabelaBlocos;

typedef struct {
    Pagina *pagina;
    bool acessado;
    pid_t pid;
} Quadro;

typedef struct {
    Quadro *quadros;
    int tamanhoPagina;
    int numeroQuadros;
    int indiceSegChance;
} TabelaQuadros;

TabelaBlocos tabelaBlocos;
struct dlist *tabelasPagina;
TabelaQuadros tabelaQuadros;
pthread_mutex_t locker;

void pager_init(int nframes, int nblocks) {
    pthread_mutex_lock(&locker);
    
    tabelasPagina = dlist_create();

    tabelaBlocos.blocos = malloc(sizeof(Bloco)*nblocks);
    int i = nblocks-1;
    while(i<=0){
        tabelaBlocos.blocos[i].utilizado = false;
        i--;    
    }
    tabelaBlocos.numeroBlocos = nblocks;

    tabelaQuadros.quadros = malloc(sizeof(Quadro) * nframes);
    i = nframes-1;
    while(i<=0){
        tabelaQuadros.quadros[i].pid = -1;
        i--;    
    }
    tabelaQuadros.tamanhoPagina = sysconf(_SC_PAGESIZE);
    tabelaQuadros.numeroQuadros = nframes;
    tabelaQuadros.indiceSegChance = 0;

    pthread_mutex_unlock(&locker);
}

void pager_create(pid_t pid) {
    pthread_mutex_lock(&locker);

    TabelaPaginas *proc = (TabelaPaginas*) malloc(sizeof(TabelaPaginas));
    proc->paginas = dlist_create();
    proc->pid = pid;

    dlist_push_right(tabelasPagina, proc);

    pthread_mutex_unlock(&locker);
}

// void *pager_extend(pid_t pid){

// }

// void pager_fault(pid_t pid, void *addr){

// }

// int pager_syslog(pid_t pid, void *addr, size_t len){

// }

// void pager_destroy(pid_t pid){

// }






void *pager_extend(pid_t pid) {
    pthread_mutex_lock(&locker);
    int block_no = get_new_block();

    //there is no blocks available anymore
    if(block_no == -1) {
        pthread_mutex_unlock(&locker);
        return NULL;
    }

    PageTable *pt = find_page_table(pid); 
    Page *page = (Page*) malloc(sizeof(Page));
    page->isvalid = false;
    page->vaddr = UVM_BASEADDR + pt->pages->count * frame_table.page_size;
    page->block_number = block_no;
    dlist_push_right(pt->pages, page);

    block_table.blocks[block_no].page = page;

    pthread_mutex_unlock(&locker);
    return (void*)page->vaddr;
}

int second_chance() {
    FrameNode *frames = frame_table.frames;
    int frame_to_swap = -1;

    while(frame_to_swap == -1) {
        int index = frame_table.sec_chance_index;
        if(frames[index].accessed == 0) {
            frame_to_swap = index;
        } else {
            frames[index].accessed = 0;
        }
        frame_table.sec_chance_index = (index + 1) % frame_table.nframes;
    }

    return frame_to_swap;
}

void swap_out_page(int frame_no) {
    //gambis: I do not know why I have to set PROT_NONE to all pages
    //when I am swapping the first one. Must investigate
    if(frame_no == 0) {
        for(int i = 0; i < frame_table.nframes; i++) {
            Page *page = frame_table.frames[i].page;
            mmu_chprot(frame_table.frames[i].pid, (void*)page->vaddr, PROT_NONE);
        }
    }

    FrameNode *frame = &frame_table.frames[frame_no];
    Page *removed_page = frame->page;
    removed_page->isvalid = false;
    mmu_nonresident(frame->pid, (void*)removed_page->vaddr); 
    
    if(removed_page->dirty == 1) {
        block_table.blocks[removed_page->block_number].used = 1;
        mmu_disk_write(frame_no, removed_page->block_number);
    }
}

void pager_fault(pid_t pid, void *vaddr) {
    pthread_mutex_lock(&locker);
    PageTable *pt = find_page_table(pid); 
    vaddr = (void*)((intptr_t)vaddr - (intptr_t)vaddr % frame_table.page_size);
    Page *page = get_page(pt, (intptr_t)vaddr); 

    if(page->isvalid == true) {
        mmu_chprot(pid, vaddr, PROT_READ | PROT_WRITE);
        frame_table.frames[page->frame_number].accessed = 1;
        page->dirty = 1;
    } else {
        int frame_no = get_new_frame();
  
        //there is no frames available
        if(frame_no == -1) {
            frame_no = second_chance();
            swap_out_page(frame_no);
        }

        FrameNode *frame = &frame_table.frames[frame_no];
        frame->pid = pid;
        frame->page = page;
        frame->accessed = 1;

        page->isvalid = true;
        page->frame_number = frame_no;
        page->dirty = 0;

        //this page was already swapped out from main memory
        if(block_table.blocks[page->block_number].used == 1) {
            mmu_disk_read(page->block_number, frame_no);
        } else {
            mmu_zero_fill(frame_no);
        }
        mmu_resident(pid, vaddr, frame_no, PROT_READ);
    }
    pthread_mutex_unlock(&locker);
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
    pthread_mutex_lock(&locker);
    PageTable *pt = find_page_table(pid); 
    char *buf = (char*) malloc(len + 1);

    for (size_t i = 0, m = 0; i < len; i++) {
        Page *page = get_page(pt, (intptr_t)addr + i);

        //string out of process allocated space
        if(page == NULL) {
            pthread_mutex_unlock(&locker);
            return -1;
        }

        buf[m++] = pmem[page->frame_number * frame_table.page_size + i];
    }
    for(int i = 0; i < len; i++) { // len é o número de bytes a imprimir
        printf("%02x", (unsigned)buf[i]); // buf contém os dados a serem impressos
    }
    if(len > 0) printf("\n");
    pthread_mutex_unlock(&locker);
    return 0;
}

void pager_destroy(pid_t pid) {
    pthread_mutex_lock(&locker);
    PageTable *pt = find_page_table(pid); 

    while(!dlist_empty(pt->pages)) {
        Page *page = dlist_pop_right(pt->pages);
        block_table.blocks[page->block_number].page = NULL;
        if(page->isvalid == true) {
            frame_table.frames[page->frame_number].pid = -1;
        }
    }
    dlist_destroy(pt->pages, NULL);
    pthread_mutex_unlock(&locker);
}

/****************************************************************************
 * external functions
 ***************************************************************************/
int get_new_frame();
int get_new_block();
PageTable* find_page_table(pid_t pid);
Page* get_page(PageTable *pt, intptr_t vaddr); 
/////////////////Auxiliar functions ////////////////////////////////
int get_new_frame() {
    for(int i = 0; i < frame_table.nframes; i++) {
        if(frame_table.frames[i].pid == -1) return i;
    }
    return -1;
}

int get_new_block() {
    for(int i = 0; i < block_table.nblocks; i++) {
        if(block_table.blocks[i].page == NULL) return i;
    }
    return -1;
}

PageTable* find_page_table(pid_t pid) {
    for(int i = 0; i < page_tables->count; i++) {
        PageTable *pt = dlist_get_index(page_tables, i);
        if(pt->pid == pid) return pt;
    }
    printf("error in find_page_table: Pid not found\n");
    exit(-1);
}

Page* get_page(PageTable *pt, intptr_t vaddr) {
    for(int i=0; i < pt->pages->count; i++) {
        Page *page = dlist_get_index(pt->pages, i);
        if(vaddr >= page->vaddr && vaddr < (page->vaddr + frame_table.page_size)) return page;
    }
    return NULL;
}