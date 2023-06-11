#include "pager.h"

#include <sys/mman.h>

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include "mmu.h"

/////////////////////////////////list////////////////////////////////////////
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
struct dlist *dlist_create(void);
void dlist_destroy(struct dlist *dl, dlist_data_func);
void *dlist_pop_right(struct dlist *dl);
void *dlist_push_right(struct dlist *dl, void *data);
int dlist_empty(struct dlist *dl);

/* gets the data at index =idx.  =idx can be negative. */
void * dlist_get_index(const struct dlist *dl, int idx);
////////////////////////////list end///////////////////////////////////////////////

typedef struct {
    bool valida;
    int num_quadro;
    int num_bloco;
    bool sujo; //when the page is dirty, it must to be wrote on the disk before swaping it
    intptr_t virt_end;
} Pagina;

typedef struct {
    pid_t proc_id;
    struct dlist *paginas;
} TabelaPagina;

typedef struct {
    pid_t proc_id;
    bool acessada; //to be used by second change algorithm
    Pagina *pagina;
} Quadro;

typedef struct {
    int num_quadros;
    int tam_pagina;
    int index_retent;
    Quadro *quadros;
} TabelaQuadro;

typedef struct {
    bool usada; //1 if the page was copied to the disk, 0 otherwise
    Pagina *pagina;
} Bloco;

typedef struct {
    int num_blocos;
    Bloco *blocos;
} TabelaBloco;

TabelaQuadro tabela_quadro;
TabelaBloco tabela_bloco;
struct dlist *tabelas_pagina;

/****************************************************************************
 * external functions
 ***************************************************************************/
int get_new_frame();
int get_new_block();
TabelaPagina* find_page_table(pid_t proc_id);
Pagina* get_page(TabelaPagina *tabPag, intptr_t virt_end); 
pthread_mutex_t locker;

void pager_init(int num_quadros, int num_blocos) {
    pthread_mutex_lock(&locker);
    tabela_quadro.num_quadros = num_quadros;
    tabela_quadro.tam_pagina = sysconf(_SC_PAGESIZE);
    tabela_quadro.index_retent = 0;

    tabela_quadro.quadros = malloc(num_quadros * sizeof(Quadro));
    for(int i = 0; i < num_quadros; i++) {
        tabela_quadro.quadros[i].proc_id = -1;
    }

    tabela_bloco.num_blocos = num_blocos;
    tabela_bloco.blocos = malloc(num_blocos * sizeof(Bloco));
    for(int i = 0; i < num_blocos; i++) {
        tabela_bloco.blocos[i].usada = false;
    }
    tabelas_pagina = dlist_create();
    pthread_mutex_unlock(&locker);
}

void pager_create(pid_t proc_id) {
    pthread_mutex_lock(&locker);
    TabelaPagina *tp = (TabelaPagina*) malloc(sizeof(TabelaPagina));
    tp->proc_id = proc_id;
    tp->paginas = dlist_create();

    dlist_push_right(tabelas_pagina, tp);
    pthread_mutex_unlock(&locker);
}

void *pager_extend(pid_t proc_id) {
    pthread_mutex_lock(&locker);
    int num_bloco = get_new_block();

    //there is no blocks available anymore
    if(num_bloco == -1) {
        pthread_mutex_unlock(&locker);
        return NULL;
    }

    TabelaPagina *tp = find_page_table(proc_id); 
    Pagina *pagina = (Pagina*) malloc(sizeof(Pagina));
    pagina->valida = false;
    pagina->virt_end = UVM_BASEADDR + tp->paginas->count * tabela_quadro.tam_pagina;
    pagina->num_bloco = num_bloco;
    dlist_push_right(tp->paginas, pagina);

    tabela_bloco.blocos[num_bloco].pagina = pagina;

    pthread_mutex_unlock(&locker);
    return (void*)pagina->virt_end;
}

int second_chance() {
    Quadro *quadros = tabela_quadro.quadros;
    int quadro_trocar = -1;

    while(quadro_trocar == -1) {
        int index = tabela_quadro.index_retent;
        if(quadros[index].acessada == false) {
            quadro_trocar = index;
        } else {
            quadros[index].acessada = false;
        }
        tabela_quadro.index_retent = (index + 1) % tabela_quadro.num_quadros;
    }

    return quadro_trocar;
}

void swap_out_page(int num_quadro) {
    //gambis: I do not know why I have to set PROT_NONE to all pages
    //when I am swapping the first one. Must investigate
    if(num_quadro == 0) {
        for(int i = 0; i < tabela_quadro.num_quadros; i++) {
            Pagina *pagina = tabela_quadro.quadros[i].pagina;
            mmu_chprot(tabela_quadro.quadros[i].proc_id, (void*)pagina->virt_end, PROT_NONE);
        }
    }

    Quadro *quadro = &tabela_quadro.quadros[num_quadro];
    Pagina *pagina_removida = quadro->pagina;
    pagina_removida->valida = false;
    mmu_nonresident(quadro->proc_id, (void*)pagina_removida->virt_end); 
    
    if(pagina_removida->sujo == true) {
        tabela_bloco.blocos[pagina_removida->num_bloco].usada = true;
        mmu_disk_write(num_quadro, pagina_removida->num_bloco);
    }
}

void pager_fault(pid_t proc_id, void *virt_end) {
    pthread_mutex_lock(&locker);
    TabelaPagina *tp = find_page_table(proc_id); 
    virt_end = (void*)((intptr_t)virt_end - (intptr_t)virt_end % tabela_quadro.tam_pagina);
    Pagina *pagina = get_page(tp, (intptr_t)virt_end); 

    if(pagina->valida == true) {
        mmu_chprot(proc_id, virt_end, PROT_READ | PROT_WRITE);
        tabela_quadro.quadros[pagina->num_quadro].acessada = true;
        pagina->sujo = true;
    } else {
        int num_quadro = get_new_frame();

        //there is no frames available
        if(num_quadro == -1) {
            num_quadro = second_chance();
            swap_out_page(num_quadro);
        }

        Quadro *quadro = &tabela_quadro.quadros[num_quadro];
        quadro->proc_id = proc_id;
        quadro->pagina = pagina;
        quadro->acessada = true;

        pagina->valida = true;
        pagina->num_quadro = num_quadro;
        pagina->sujo = false;

        //this page was already swapped out from main memory
        if(tabela_bloco.blocos[pagina->num_bloco].usada == true) {
            mmu_disk_read(pagina->num_bloco, num_quadro);
        } else {
            mmu_zero_fill(num_quadro);
        }
        mmu_resident(proc_id, virt_end, num_quadro, PROT_READ);
    }
    pthread_mutex_unlock(&locker);
}

int pager_syslog(pid_t proc_id, void *endereco, size_t tamanho) {
    pthread_mutex_lock(&locker);
    TabelaPagina *tp = find_page_table(proc_id); 
    char *buf = (char*) malloc(tamanho + 1);

    for (size_t i = 0, m = 0; i < tamanho; i++) {
        Pagina *pagina = get_page(tp, (intptr_t)endereco + i);

        //string out of process allocated space
        if(pagina == NULL) {
            pthread_mutex_unlock(&locker);
            return -1;
        }

        buf[m++] = pmem[pagina->num_quadro * tabela_quadro.tam_pagina + i];
    }
    for(int i = 0; i < tamanho; i++) { // len é o número de bytes a imprimir
        printf("%02x", (unsigned)buf[i]); // buf contém os dados a serem impressos
    }
    if(tamanho > 0) printf("\n");
    pthread_mutex_unlock(&locker);
    return 0;
}

void pager_destroy(pid_t proc_id) {
    pthread_mutex_lock(&locker);
    TabelaPagina *tp = find_page_table(proc_id); 

    while(!dlist_empty(tp->paginas)) {
        Pagina *pagina = dlist_pop_right(tp->paginas);
        tabela_bloco.blocos[pagina->num_bloco].pagina = NULL;
        if(pagina->valida == true) {
            tabela_quadro.quadros[pagina->num_quadro].proc_id = -1;
        }
    }
    dlist_destroy(tp->paginas, NULL);
    pthread_mutex_unlock(&locker);
}

/////////////////Auxiliar functions ////////////////////////////////
int get_new_frame() {
    for(int i = 0; i < tabela_quadro.num_quadros; i++) {
        if(tabela_quadro.quadros[i].proc_id == -1) return i;
    }
    return -1;
}

int get_new_block() {
    for(int i = 0; i < tabela_bloco.num_blocos; i++) {
        if(tabela_bloco.blocos[i].pagina == NULL) return i;
    }
    return -1;
}

TabelaPagina* find_page_table(pid_t proc_id) {
    for(int i = 0; i < tabelas_pagina->count; i++) {
        TabelaPagina *tp = dlist_get_index(tabelas_pagina, i);
        if(tp->proc_id == proc_id) return tp;
    }
    printf("error in find_page_table: Pid not found\n");
    exit(-1);
}

Pagina* get_page(TabelaPagina *tp, intptr_t virt_end) {
    for(int i=0; i < tp->paginas->count; i++) {
        Pagina *pagina = dlist_get_index(tp->paginas, i);
        if(virt_end >= pagina->virt_end && virt_end < (pagina->virt_end + tabela_quadro.tam_pagina)) return pagina;
    }
    return NULL;
}

/////////////////////// List functions //////////////////////////////
struct dlist *dlist_create(void) {
    struct dlist *dl = malloc(sizeof(struct dlist));
    assert(dl);
    dl->head = NULL;
    dl->tail = NULL;
    dl->count = 0;
    return dl;
}

void dlist_destroy(struct dlist *dl, dlist_data_func cb) {
    while(!dlist_empty(dl)) {
        void *data = dlist_pop_right(dl);
        if(cb) cb(data);
    }
    free(dl);
}

void *dlist_pop_right(struct dlist *dl) {
    if(dlist_empty(dl)) return NULL;

    void *data;
    struct dnode *node;

    node = dl->tail;

    dl->tail = node->prev;
    if(dl->tail == NULL) dl->head = NULL;
    if(node->prev) node->prev->next = NULL;

    data = node->data;
    free(node);

    dl->count--;
    assert(dl->count >= 0);

    return data;
}

void *dlist_push_right(struct dlist *dl, void *data) {
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
}

int dlist_empty(struct dlist *dl) {
    int ret;
    if(dl->head == NULL) {
        assert(dl->tail == NULL);
        assert(dl->count == 0);
        ret = 1;
    } else {
        assert(dl->tail != NULL);
        assert(dl->count > 0);
        ret = 0;
    }
    return ret;
}

void * dlist_get_index(const struct dlist *dl, int idx) {
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
}