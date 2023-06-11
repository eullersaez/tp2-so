#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mmu.h"
#include "pager.h"

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

void * dlist_get_index(const struct dlist *dl, int idx);

typedef struct {
    bool valida;
    int num_quadro;
    int num_pagina;
    bool sujo; 
    intptr_t virt_end;
    bool usada; 
} Pagina;

typedef struct {
    pid_t proc_id;
    struct dlist *paginas;
} TabelaPagina;

typedef struct {
    pid_t proc_id;
    bool acessada;
    Pagina *pagina;
} Quadro;

typedef struct {
    int num_quadros;
    int tam_pagina;
    int index_retent;
    Quadro *quadros;
} TabelaQuadro;

typedef struct {
    int num_paginas;
    Pagina *paginas;
} TabelaBloco;

TabelaQuadro tabela_quadro;
TabelaBloco tabela_bloco;
struct dlist *tabelas_pagina;

TabelaPagina* get_tp(pid_t proc_id);
pthread_mutex_t locker;

void pager_init(int num_quadros, int num_paginas) {
    pthread_mutex_lock(&locker);
    tabela_quadro.num_quadros = num_quadros;
    tabela_quadro.tam_pagina = sysconf(_SC_PAGESIZE);
    tabela_quadro.index_retent = 0;

    tabela_quadro.quadros = malloc(num_quadros * sizeof(Quadro));
    for(int i = 0; i < num_quadros; i++) {
        tabela_quadro.quadros[i].proc_id = -1;
    }

    tabela_bloco.num_paginas = num_paginas;
    tabela_bloco.paginas = malloc(num_paginas * sizeof(Pagina));
    for(int i = 0; i < num_paginas; i++) {
        tabela_bloco.paginas[i].usada = false;
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
    int num_pagina = 0;

    while(num_pagina < tabela_bloco.num_paginas) {
        if(!tabela_bloco.paginas[num_pagina].virt_end) break;
        num_pagina++;
    }

    if(num_pagina == tabela_bloco.num_paginas) {
        pthread_mutex_unlock(&locker);
        return NULL;
    }

    TabelaPagina *tp = get_tp(proc_id); 
    Pagina *pagina = (Pagina*) malloc(sizeof(Pagina));
    pagina->valida = false;
    pagina->virt_end = UVM_BASEADDR + tp->paginas->count * tabela_quadro.tam_pagina;
    pagina->num_pagina = num_pagina;
    dlist_push_right(tp->paginas, pagina);

    tabela_bloco.paginas[num_pagina] = *pagina;

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
        tabela_bloco.paginas[pagina_removida->num_pagina].usada = true;
        mmu_disk_write(num_quadro, pagina_removida->num_pagina);
    }
}

void pager_fault(pid_t proc_id, void *virt_end) {
    pthread_mutex_lock(&locker);
    TabelaPagina *tp = get_tp(proc_id); 
    virt_end = (void*)((intptr_t)virt_end - (intptr_t)virt_end % tabela_quadro.tam_pagina);
    Pagina *pagina = NULL; 

    for(int i=0; i < tp->paginas->count; i++) {
        intptr_t aux_end = (intptr_t)virt_end;
        Pagina *aux = dlist_get_index(tp->paginas, i);
        if(aux_end >= aux->virt_end && aux_end < (aux->virt_end + tabela_quadro.tam_pagina)) {
            pagina = aux;
            break;
        }
    }
    
    if(pagina->valida == true) {
        mmu_chprot(proc_id, virt_end, PROT_READ | PROT_WRITE);
        tabela_quadro.quadros[pagina->num_quadro].acessada = true;
        pagina->sujo = true;
    } else {
        int num_quadro = 0;

        while(num_quadro < tabela_quadro.num_quadros) {
            if(tabela_quadro.quadros[num_quadro].proc_id == -1) break;
            num_quadro++;
        }

        if(num_quadro == tabela_quadro.num_quadros) {
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

        if(tabela_bloco.paginas[pagina->num_pagina].usada == true) {
            mmu_disk_read(pagina->num_pagina, num_quadro);
        } else {
            mmu_zero_fill(num_quadro);
        }
        mmu_resident(proc_id, virt_end, num_quadro, PROT_READ);
    }
    pthread_mutex_unlock(&locker);
}

int pager_syslog(pid_t proc_id, void *endereco, size_t tamanho) {
    pthread_mutex_lock(&locker);
    TabelaPagina *tp = get_tp(proc_id); 
    char *buf = (char*) malloc(tamanho + 1);

    for (size_t i = 0, m = 0; i < tamanho; i++) {
        Pagina *pagina = NULL;

        for(int i=0; i < tp->paginas->count; i++) {
            intptr_t aux_end = (intptr_t)endereco;
            Pagina *aux = dlist_get_index(tp->paginas, i);
            if(aux_end >= aux->virt_end && aux_end < (aux->virt_end + tabela_quadro.tam_pagina)) {
                pagina = aux;
                break;
            }
        }
        if(pagina == NULL) {
            pthread_mutex_unlock(&locker);
            return -1;
        }

        buf[m++] = pmem[pagina->num_quadro * tabela_quadro.tam_pagina + i];
    }
    for(int i = 0; i < tamanho; i++) { 
        printf("%02x", (unsigned)buf[i]); 
    }
    if(tamanho > 0) printf("\n");
    pthread_mutex_unlock(&locker);
    return 0;
}

void pager_destroy(pid_t proc_id) {
    pthread_mutex_lock(&locker);
    TabelaPagina *tp = get_tp(proc_id); 

    while(!dlist_empty(tp->paginas)) {
        Pagina *pagina = dlist_pop_right(tp->paginas);
        if(pagina->valida == true) {
            tabela_quadro.quadros[pagina->num_quadro].proc_id = -1;
        }
    }
    dlist_destroy(tp->paginas, NULL);
    pthread_mutex_unlock(&locker);
}

TabelaPagina* get_tp(pid_t proc_id) {
    for(int i = 0; i < tabelas_pagina->count; i++) {
        TabelaPagina *tp = dlist_get_index(tabelas_pagina, i);
        if(tp->proc_id == proc_id) return tp;
    }
    exit(-1);
}

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