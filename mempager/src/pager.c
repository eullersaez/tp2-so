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

void *pager_extend(pid_t pid) {
    pthread_mutex_lock(&locker);

    int numeroBloco = 0;
    while(numeroBloco<tabelaBlocos.numeroBlocos){
        if(tabelaBlocos.blocos[numeroBloco].pagina == NULL){
            break;
        }
        numeroBloco++;
    }
    /* Nesse caso não hã mais blocos disponíveis */
    if (numeroBloco == tabelaBlocos.numeroBlocos + 1){
        pthread_mutex_unlock(&locker);
        return NULL;
    }

    TabelaPaginas *proc;
    int i = 0;
    while (i <= tabelasPagina->count-1)
    {
        proc = dlist_get_index(tabelasPagina, i);
        if(proc->pid == pid){
            break;
        } 
        i++;
    }

    Pagina *pagina = (Pagina*) malloc(sizeof(Pagina));
    pagina->vaddr = UVM_BASEADDR + proc->paginas->count * tabelaQuadros.tamanhoPagina;
    pagina->numeroBloco = numeroBloco;
    pagina->valida = false;
    dlist_push_right(proc->paginas, pagina);

    tabelaBlocos.blocos[numeroBloco].pagina = pagina;

    pthread_mutex_unlock(&locker);
    return (void*)pagina->vaddr;
}

void pager_fault(pid_t pid, void *vaddr) {
    pthread_mutex_lock(&locker);

    /* Obtem a tabela de pag do processo */
    TabelaPaginas *proc;
    int i = 0;
    while (i <= tabelasPagina->count-1)
    {
        proc = dlist_get_index(tabelasPagina, i);
        if(proc->pid == pid){
            break;
        } 
        i++; 
    }

    vaddr = (void*)((intptr_t)vaddr - (intptr_t)vaddr % tabelaQuadros.tamanhoPagina);

    /* Pegando a pagina correspondente */
    Pagina *pagina;
    i = 0;
    while (i<=proc->paginas->count-1)
    {
        pagina = dlist_get_index(proc->paginas, i);
        if(pagina->vaddr <= (intptr_t)vaddr && (tabelaQuadros.tamanhoPagina + pagina->vaddr) > (intptr_t)vaddr){
            break;
        }
        else{
            pagina = NULL;
        }
        i++;
    }

    if(pagina->valida == true) {
        mmu_chprot(pid, vaddr, PROT_READ | PROT_WRITE);
        pagina->modificada = true;
        tabelaQuadros.quadros[pagina->numeroQuadro].acessado = true;

    } else {
        /* Pegando novo quadro */
        int numeroQuadro = -1;
        i = 0;
        while(i<=tabelaQuadros.numeroQuadros-1){
            if (tabelaQuadros.quadros[i].pid == -1){
                numeroQuadro = i;
                break;
            }
            i++;
        }
  
        /* No caso de nao terem mais quadros disponiveis */
        if(numeroQuadro == -1) {
            /* Aplica-se o alg da segunda chance */
            Quadro *quadros = tabelaQuadros.quadros;
            while(numeroQuadro == -1) {
                int indice = tabelaQuadros.indiceSegChance;
                if(quadros[indice].acessado == false) {
                    numeroQuadro = indice;
                } else {
                    quadros[indice].acessado = false;
                }
                tabelaQuadros.indiceSegChance = (1 + indice) % tabelaQuadros.numeroQuadros;
            }

            /* Trocando a pagina */
            if(numeroQuadro == 0) {
                i = 0;
                while(i<=tabelaQuadros.numeroQuadros-1){
                    Pagina *pagina = tabelaQuadros.quadros[i].pagina;
                    mmu_chprot(tabelaQuadros.quadros[i].pid, (void*)pagina->vaddr, PROT_NONE);
                    i++;
                }
            }

            Quadro *quadro = &tabelaQuadros.quadros[numeroQuadro];
            Pagina *paginaRemovida = quadro->pagina;
            paginaRemovida->valida = false;
            mmu_nonresident(quadro->pid, (void*)paginaRemovida->vaddr); 
            
            if(paginaRemovida->modificada == true) {
                tabelaBlocos.blocos[paginaRemovida->numeroBloco].utilizado = true;
                mmu_disk_write(numeroQuadro, paginaRemovida->numeroBloco);
            }
        }

        Quadro *quadro = &tabelaQuadros.quadros[numeroQuadro];
        quadro->acessado = true;
        quadro->pid = pid;
        quadro->pagina = pagina;

        pagina->valida = true;
        pagina->modificada = false;
        pagina->numeroQuadro = numeroQuadro;

        /* Pagina ja trocada da memoria */
        if(tabelaBlocos.blocos[pagina->numeroBloco].utilizado == true) {
            mmu_disk_read(pagina->numeroBloco, numeroQuadro);
        } else {
            mmu_zero_fill(numeroQuadro);
        }
        mmu_resident(pid, vaddr, numeroQuadro, PROT_READ);
    }
    pthread_mutex_unlock(&locker);
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
    pthread_mutex_lock(&locker);

    /* Busca pela tabela de pag do proc */
    TabelaPaginas *proc;
    int i = 0;
    while (i <= tabelasPagina->count-1){
        proc = dlist_get_index(tabelasPagina, i);
        if(proc->pid == pid){
            break;
        }
        i++;
    }

    char *data = (char*) malloc(1 + len);

    for (size_t i = 0, m = 0; i < len; i++) {
        /* Pegando a pagina correspondente */
        Pagina *pagina;
        int j = 0;
        while (j<=proc->paginas->count-1)
        {
            pagina = dlist_get_index(proc->paginas, j);
            if(pagina->vaddr <= (intptr_t)addr+i && (tabelaQuadros.tamanhoPagina + pagina->vaddr) > (intptr_t)addr+i){
                break;
            }
            else{
                pagina = NULL;
            }
            j++;
        }

        /* Fora do espaço alocado */
        if(pagina == NULL) {
            pthread_mutex_unlock(&locker);
            return -1;
        }

        data[m++] = pmem[tabelaQuadros.tamanhoPagina + i * pagina->numeroQuadro];
    }

    for(int i = 0; i < len; i++) {
        printf("%02x", (unsigned)data[i]);
    }

    if(len > 0) {
        printf("\n");
    };

    pthread_mutex_unlock(&locker);
    return 0;
}

void pager_destroy(pid_t pid) {
    pthread_mutex_lock(&locker);

    /* Encontra a tabela de pag do proc */
    TabelaPaginas *proc;
    int i = 0;
    while (i <= tabelasPagina->count-1)
    {
        proc = dlist_get_index(tabelasPagina, i);
        if(proc->pid == pid){
            break;
        }else{
            proc = NULL;
        }
        i++;
    }

    while(!dlist_empty(proc->paginas)) {
        Pagina *pagina = dlist_pop_right(proc->paginas);
        tabelaBlocos.blocos[pagina->numeroBloco].pagina = NULL;
        if(pagina->valida == true) {
            tabelaQuadros.quadros[pagina->numeroQuadro].pid = -1;
        }
    }
    dlist_destroy(proc->paginas, NULL);

    pthread_mutex_unlock(&locker);
}