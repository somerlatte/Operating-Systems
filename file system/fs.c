#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

#include "fs.h"

#define LINK_MAX (sb->blksz - 32) / sizeof(uint64_t)
#define NAME_MAX sb->blksz - (8 * sizeof(uint64_t))

//verifica se um inode possui links
int verificaPresencaLinks(struct superblock *sb, uint64_t thisblk);

struct dir {
	uint64_t dirnode;   
	uint64_t nodeblock; 
	char *nodename;     
};

struct link {
	uint64_t inode; 
	int index;      
};

//tamanho de um arquivo
int obterTamanhoDoArquivo(const char *fname) {
	int sz;
	FILE *fd = fopen(fname, "r");
	fseek(fd, 0L, SEEK_END);
	sz = ftell(fd);
	rewind(fd);
	fclose(fd);
	return sz;
}

//escrever dados
void escreverDadosNoSistemaDeArquivos(struct superblock *sb, uint64_t pos, void *data) {


	lseek(sb->fd, pos * sb->blksz, SEEK_SET);
	write(sb->fd, data, sb->blksz);
}

//ler dados
void lerDadosDoSistemaDeArquivos(struct superblock *sb, uint64_t pos, void *data) {
	lseek(sb->fd, pos * sb->blksz, SEEK_SET);
	read(sb->fd, data, sb->blksz);
}

struct dir * encontrarInformacoesDoDiretorio(struct superblock *sb, const char *dpath) {
	int pathlenght = 0;
	char *token;
	char *nodename = malloc(NAME_MAX);
	char *pathcopy = malloc(NAME_MAX);
	struct dir *dir = malloc(sizeof *dir); 

	strcpy(pathcopy, dpath);

	token = strtok(pathcopy, "/");
	if(token == NULL) {
		dir->dirnode = 1;
		dir->nodeblock = 1;
		dir->nodename = "";
		return dir;
	}

	while(token != NULL) {
		strcpy(nodename, token);
		pathlenght++;
		token = strtok(NULL, "/");
	} 

	strcpy(pathcopy, dpath);

	uint64_t dirnode, nodeblock, j;
	struct inode *inode          = malloc(sb->blksz);
	struct inode *auxinode       = malloc(sb->blksz);
	struct nodeinfo *nodeinfo    = malloc(sb->blksz);
	struct nodeinfo *auxnodeinfo = malloc(sb->blksz);

	dirnode = 1;

	lerDadosDoSistemaDeArquivos(sb, dirnode, (void*) inode);
	lerDadosDoSistemaDeArquivos(sb, inode->meta, (void*) nodeinfo);
	token = strtok(pathcopy, "/");

	j = 0;

	for(int i = 0; i < pathlenght; i++) {
		while(j < LINK_MAX) {
			nodeblock = inode->links[j];
			if(nodeblock != 0) {
				lerDadosDoSistemaDeArquivos(sb, nodeblock, (void*) auxinode);
				lerDadosDoSistemaDeArquivos(sb, auxinode->meta, (void*) auxnodeinfo);

				if(!strcmp(auxnodeinfo->name, token)) {
					if(i + 1 < pathlenght) dirnode = nodeblock;
					inode = auxinode;
					nodeinfo = auxnodeinfo;
					break;
				}	
			}

			j++;

			if(j == LINK_MAX) {
				if(inode->next != 0) { 
					j = 0;
					lerDadosDoSistemaDeArquivos(sb, inode->next, (void*)inode);
				}
				else{ 
					if(i + 1 == pathlenght) {
						nodeblock = -1; 
						break;
					}
					else { 
						free(dir);
						free(nodename);
						free(pathcopy);
						free(inode);
						free(nodeinfo);
						errno = ENOENT;
						return NULL;
					}
				}
			}
		}
		j = 0;
		token = strtok(NULL, "/");
	}

	dir->dirnode = dirnode;
	dir->nodeblock = nodeblock;
	dir->nodename = malloc(NAME_MAX);
	strcpy(dir->nodename, nodename);

	free(nodename);
	free(pathcopy);
	free(inode);
	free(nodeinfo);

	return dir;
}

//encontrar um link em um inode
struct link * encontrarLink(struct superblock *sb, uint64_t inodeblk, uint64_t linkvalue) {
	int i = 0;
	uint64_t actualblk = inodeblk;
	struct link *link = malloc(sizeof *link);
	struct inode *inode = malloc(sb->blksz);

	lerDadosDoSistemaDeArquivos(sb, inodeblk, (void*) inode);

	while(i < LINK_MAX) {
		if(inode->links[i] == linkvalue) {
			link->inode = actualblk;
			link->index = i;
			break;
		}

		i++;

		if(i == LINK_MAX) {
			if(inode->next == 0) {
				link->inode = actualblk;
				link->index = -1;
				break;
			}
			else{ 
				i = 0;
				actualblk = inode->next;
				lerDadosDoSistemaDeArquivos(sb, inode->next, (void*) inode);
			}
		}
	}

	free(inode);

	return link;
}

// cria um novo bloco 
uint64_t criarFilho(struct superblock *sb, uint64_t thisblk, uint64_t parentblk) {
    uint64_t arq;
    struct inode *inode = malloc(sb->blksz);
    struct inode *childnode = malloc(sb->blksz);

    lerDadosDoSistemaDeArquivos(sb, thisblk, (void*) inode);

    inode->next = fs_get_block(sb);

    childnode->mode   = IMCHILD;
    childnode->parent = parentblk;
    childnode->meta   = thisblk;
    childnode->next   = 0;
    for (int i = 0; i < LINK_MAX; i++) {
        childnode->links[i] = 0;
    }

    escreverDadosNoSistemaDeArquivos(sb, thisblk, (void*) inode);
    escreverDadosNoSistemaDeArquivos(sb, inode->next, (void*) childnode);

    arq = inode->next;

    free(inode);
    free(childnode);

    return arq;
}

// adiciona um novo link a
void adicionarLink(struct superblock *sb, uint64_t parentblk, int linkindex, uint64_t newlink) {
    uint64_t nodeinfoblk;
    struct inode *inode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);

    lerDadosDoSistemaDeArquivos(sb, parentblk, (void*) inode);
    nodeinfoblk = inode->meta;
    if (inode->mode == IMCHILD) {
        struct inode *parentnode = malloc(sb->blksz);
        lerDadosDoSistemaDeArquivos(sb, inode->parent, parentnode);
        nodeinfoblk = parentnode->meta;
        free(parentnode);
    }
    lerDadosDoSistemaDeArquivos(sb, nodeinfoblk, (void*) nodeinfo);

    inode->links[linkindex] = newlink;
    nodeinfo->size++;

    escreverDadosNoSistemaDeArquivos(sb, parentblk, (void*) inode);
    escreverDadosNoSistemaDeArquivos(sb, nodeinfoblk, (void*) nodeinfo);

    free(inode);
}

// remove um link 
void removerLink(struct superblock *sb, uint64_t parentblk, int linkindex) {
    uint64_t nodeinfoblk;
    struct inode *inode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);

    lerDadosDoSistemaDeArquivos(sb, parentblk, (void*) inode);
    nodeinfoblk = inode->meta;
    if (inode->mode == IMCHILD) {
        struct inode *parentnode = malloc(sb->blksz);
        lerDadosDoSistemaDeArquivos(sb, inode->parent, (void*) parentnode);
        nodeinfoblk = parentnode->meta;
        free(parentnode);
    }
    lerDadosDoSistemaDeArquivos(sb, nodeinfoblk, (void*) nodeinfo);

    inode->links[linkindex] = 0;
    nodeinfo->size--;

    if (inode->mode == IMCHILD && inode->next == 0 && !verificaPresencaLinks(sb, parentblk)) {
        fs_put_block(sb, parentblk);
        struct inode *previousnode = malloc(sb->blksz);
        lerDadosDoSistemaDeArquivos(sb, inode->meta, (void*) previousnode);
        previousnode->next = 0;
        escreverDadosNoSistemaDeArquivos(sb, inode->meta, (void*) previousnode);
        free(previousnode);
    } else {
        escreverDadosNoSistemaDeArquivos(sb, parentblk, (void*) inode);
    }
    escreverDadosNoSistemaDeArquivos(sb, nodeinfoblk, (void*) nodeinfo);

    free(inode);
}

// verifica se um bloco possui links
int verificaPresencaLinks(struct superblock *sb, uint64_t thisblk) {
    int arq;
    struct inode *inode = malloc(sb->blksz);

    lerDadosDoSistemaDeArquivos(sb, thisblk, (void*) inode);

    for (int i = 0; i < LINK_MAX; i++) {
        arq = inode->links[i] ? 1 : 0;
    }

    free(inode);

    return arq;
}

//funções
struct superblock *fs_format(const char *fname, uint64_t blocksize) {
    // verifica o tamanho do bloco mínimo
    if (blocksize < MIN_BLOCK_SIZE) {
        errno = EINVAL;
        return NULL;
    }

    // aloca estruturas
    struct superblock *sb = malloc(sizeof *sb);
    struct inode *rootnode = malloc(blocksize);
    struct nodeinfo *rootinfo = malloc(blocksize);
    struct freepage *freepage = malloc(blocksize);

    // inicializa informações do superbloco
    sb->magic = 0xdcc605f5;
    sb->blks = obterTamanhoDoArquivo(fname) / blocksize;
    sb->blksz = blocksize;
    sb->freeblks = sb->blks - 3;
    sb->freelist = 3;
    sb->root = 1;
    sb->fd = open(fname, O_RDWR, 0666);
    rootnode->mode = IMDIR;
    rootnode->parent = 1;
    rootnode->meta = 2;
    rootnode->next = 0;
    for (int i = 0; i < LINK_MAX; i++) {
        rootnode->links[i] = 0;
    }

    rootinfo->size = 0;
    strcpy(rootinfo->name, "/");

    // bloqueia o arquivo para garantir operação exclusiva
    if (flock(sb->fd, LOCK_EX | LOCK_NB) == -1) {
        errno = EBUSY;
        return NULL;
    }

    // escreve no disco
    escreverDadosNoSistemaDeArquivos(sb, 0, (void *)sb);
    escreverDadosNoSistemaDeArquivos(sb, 1, (void *)rootnode);
    escreverDadosNoSistemaDeArquivos(sb, 2, (void *)rootinfo);

    // inicializa e escreve blocos livres
    for (uint64_t i = 3; i < sb->blks; i++) {
        freepage->next = (i + 1 == sb->blks) ? 0 : i + 1;
        freepage->count = 0;
        escreverDadosNoSistemaDeArquivos(sb, i, (void *)freepage);
    }

    // libera memória
    free(rootnode);
    free(rootinfo);
    free(freepage);

    // verifica se há espaço no disco
    if (sb->blks < MIN_BLOCK_COUNT) {
        close(sb->fd);
        free(sb);
        errno = ENOSPC;
        return NULL;
    }

    return sb;
}

// abre o sistema de arquivos e
struct superblock *fs_open(const char *fname) {
    struct superblock *sb = malloc(sizeof *sb);
    int fd = open(fname, O_RDWR, 0666);

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        errno = EBUSY;
        return NULL;
    }

    read(fd, sb, sizeof *sb);
    sb->fd = fd;

    if (sb->magic != 0xdcc605f5) {
        errno = EBADF;
        return NULL;
    }

    return sb;
}

// fecha o sistema de arquivos
int fs_close(struct superblock *sb) {
    if (sb->magic != 0xdcc605f5) {
        errno = EBADF;
        return -1;
    }

    flock(sb->fd, LOCK_UN);
    close(sb->fd);
    free(sb);

    return 0;
}

// bloco livre
uint64_t fs_get_block(struct superblock *sb) {
    if (sb->freeblks == 0) {
        return 0;
    }

    if (sb->magic != 0xdcc605f5) {
        errno = EBADF;
        return (uint64_t)-1;
    }

    uint64_t arq;
    struct freepage *freepage = malloc(sb->blksz);
    lerDadosDoSistemaDeArquivos(sb, sb->freelist, (void *)freepage);
    arq = sb->freelist;
    sb->freeblks--;
    sb->freelist = freepage->next;
    escreverDadosNoSistemaDeArquivos(sb, 0, (void *)sb);

    free(freepage);

    return arq;
}

// libera um bloco
int fs_put_block(struct superblock *sb, uint64_t block) {
    if (sb->magic != 0xdcc605f5) {
        errno = EBADF;
        return -1;
    }

    struct freepage *freepage = malloc(sb->blksz);
    freepage->next = sb->freelist;
    freepage->count = 0;
    sb->freeblks++;
    sb->freelist = block;
    escreverDadosNoSistemaDeArquivos(sb, block, (void *)freepage);
    escreverDadosNoSistemaDeArquivos(sb, 0, (void *)sb);

    free(freepage);

    return 0;
}

// escreve dados em um arquivo
int fs_write_file(struct superblock *sb, const char *fname, char *buf, size_t cnt) {
    uint64_t datablks, extrainodes, neededblks, links;
    uint64_t fileblk, previousblk, linkblk;
    struct dir *dir;
    struct link *link;
    struct inode *inode = malloc(sb->blksz);
    struct inode *childnode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);

    // calcula o número de blocos
    datablks = (cnt / sb->blksz) + ((cnt % sb->blksz) ? 1 : 0);
    extrainodes = 0;

    if (datablks > LINK_MAX) {
        extrainodes = (datablks / LINK_MAX) + (datablks % LINK_MAX ? 1 : 0);
    }

    dir = encontrarInformacoesDoDiretorio(sb, fname);

    if (dir == NULL) {
        free(dir);
        free(inode);
        free(childnode);
        free(nodeinfo);
        return -1;
    }

    if (dir->nodeblock != -1) {
        fs_unlink(sb, fname);
    }

    link = encontrarLink(sb, dir->dirnode, 0);
    neededblks = datablks + 2 + extrainodes + (link->index == -1 ? 1 : 0);

    // verifica se há blocos livres suficientes
    if (neededblks > sb->freeblks) {
        free(dir);
        free(link);
        free(inode);
        free(childnode);
        free(nodeinfo);
        errno = ENOSPC;
        return -1;
    }

    fileblk = fs_get_block(sb);

    if (link->index == -1) {
        adicionarLink(sb, criarFilho(sb, link->inode, dir->dirnode), 0, fileblk);
    } else {
        adicionarLink(sb, link->inode, link->index, fileblk);
    }

    inode->mode = IMREG;
    inode->parent = dir->dirnode;
    inode->meta = fs_get_block(sb);
    inode->next = 0;

    escreverDadosNoSistemaDeArquivos(sb, fileblk, (void *)inode);

    links = (datablks > LINK_MAX) ? LINK_MAX : datablks;
    for (int i = 0; i < LINK_MAX; i++) {
        if (i < links) {
            linkblk = fs_get_block(sb);
            escreverDadosNoSistemaDeArquivos(sb, linkblk, (void *)(buf + i * sb->blksz));
            inode->links[i] = linkblk;
        } else {
            inode->links[i] = 0;
        }
    }
    datablks -= links;

    
    previousblk = fileblk;
    for (int i = 0; i < extrainodes; i++) {
        previousblk = criarFilho(sb, previousblk, fileblk);
        lerDadosDoSistemaDeArquivos(sb, previousblk, (void *)childnode);
        links = (datablks > LINK_MAX) ? LINK_MAX : datablks;

        for (int j = 0; j < LINK_MAX; j++) {
            if (j < links) {
                linkblk = fs_get_block(sb);
                escreverDadosNoSistemaDeArquivos(sb, linkblk, (void *)(buf + i * LINK_MAX * sb->blksz + j * sb->blksz));
                childnode->links[j] = linkblk;
            } else {
                childnode->links[j] = 0;
            }
        }
        // escreve os dados do inode extra no disco
        escreverDadosNoSistemaDeArquivos(sb, previousblk, (void *)childnode);
        datablks -= links;
    }

    nodeinfo->size = cnt;
    strcpy(nodeinfo->name, dir->nodename);
    escreverDadosNoSistemaDeArquivos(sb, fileblk, (void *)inode);
    escreverDadosNoSistemaDeArquivos(sb, inode->meta, (void *)nodeinfo);

    // libera a memória alocada
    free(dir);
    free(link);
    free(inode);
    free(childnode);
    free(nodeinfo);

    return 0;
}

// lê dados de um arquivo
ssize_t fs_read_file(struct superblock *sb, const char *fname, char *buf, size_t bufsz) {
    size_t numblks;
    struct dir *dir;
    struct inode *inode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);
    dir = encontrarInformacoesDoDiretorio(sb, fname);

    if (dir->nodeblock == -1) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = ENOENT;
        return -1;
    }

    lerDadosDoSistemaDeArquivos(sb, dir->nodeblock, (void *)inode);
    lerDadosDoSistemaDeArquivos(sb, inode->meta, (void *)nodeinfo);

    if (inode->mode != IMREG) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = EISDIR;
        return -1;
    }

    if (bufsz > nodeinfo->size)
        bufsz = nodeinfo->size;

    // calcula o número de blocos necessários para leitura
    numblks = (bufsz / sb->blksz) + ((bufsz % sb->blksz) ? 1 : 0);

    // lê os dados dos blocos de links
    for (int i = 0; i < numblks; i++) {
        if ((i != 0) && (i % LINK_MAX == 0)) {
            if (inode->next != 0) {
                lerDadosDoSistemaDeArquivos(sb, inode->next, (void *)inode);
            } else {
                free(dir);
                free(inode);
                free(nodeinfo);
                errno = EPERM;
                return -1;
            }
        }

        lerDadosDoSistemaDeArquivos(sb, inode->links[i % LINK_MAX], (void *)(buf + i * sb->blksz));
    }

    // libera a memória alocada
    free(dir);
    free(inode);
    free(nodeinfo);

    return bufsz;
}

// remove um arquivo
int fs_unlink(struct superblock *sb, const char *fname) {
    int numblks, numlinks;
    uint64_t thisblk, nextblk;
    struct dir *dir;
    struct link *link;
    struct inode *inode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);

    dir = encontrarInformacoesDoDiretorio(sb, fname);

    if (dir->nodeblock == -1) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = ENOENT;
        return -1;
    }

    lerDadosDoSistemaDeArquivos(sb, dir->nodeblock, (void *)inode);
    lerDadosDoSistemaDeArquivos(sb, inode->meta, (void *)nodeinfo);

    if (inode->mode != IMREG) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = ENOENT;
        return -1;
    }

    // calcula o número total de blocos de dados
    numblks = (nodeinfo->size / sb->blksz) + ((nodeinfo->size % sb->blksz) ? 1 : 0);
    numlinks = (numblks > LINK_MAX) ? LINK_MAX : numblks;

    // libera os blocos de dados
    for (int i = 0; i < numlinks; i++) {
        fs_put_block(sb, inode->links[i]);
    }
    numblks -= numlinks;
    nextblk = inode->next;
    fs_put_block(sb, dir->nodeblock);
    fs_put_block(sb, inode->meta);

    while (nextblk != 0) {
        lerDadosDoSistemaDeArquivos(sb, nextblk, inode);
        thisblk = nextblk;
        nextblk = inode->next;

        numlinks = (numblks > LINK_MAX) ? LINK_MAX : numblks;
        for (int i = 0; i < numlinks; i++) {
            fs_put_block(sb, inode->links[i]);
        }
        numblks -= numlinks;

        fs_put_block(sb, thisblk);
        fs_put_block(sb, inode->meta);
    }

    link = encontrarLink(sb, dir->dirnode, dir->nodeblock);

    // remove o link do diretório
    removerLink(sb, link->inode, link->index);

    // libera a memória alocada
    free(dir);
    free(link);
    free(inode);
    free(nodeinfo);

    return 0;
}

// cria um diretório
int fs_mkdir(struct superblock *sb, const char *dname) {
    uint64_t dirblk;
    struct dir *dir;
    struct link *link;
    struct inode *inode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);
    dir = encontrarInformacoesDoDiretorio(sb, dname);

    if (dir == NULL) {
        free(dir);
        free(inode);
        free(nodeinfo);
        return -1;
    }

    if (dir->nodeblock != -1) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = EEXIST;
        return -1;
    }

    link = encontrarLink(sb, dir->dirnode, 0);

    // verifica se há blocos livres suficientes para criar o diretório
    if ((sb->freeblks < (2 + (link->index == -1 ? 1 : 0)))) {
        free(dir);
        free(link);
        free(inode);
        free(nodeinfo);
        errno = ENOSPC;
        return -1;
    }

    dirblk = fs_get_block(sb);

    if (link->index == -1) {
        adicionarLink(sb, criarFilho(sb, link->inode, dir->dirnode), 0, dirblk);
    } else {
        adicionarLink(sb, link->inode, link->index, dirblk);
    }

    inode->mode = IMDIR;
    inode->parent = dir->dirnode;
    inode->meta = fs_get_block(sb);
    inode->next = 0;
    for (int i = 0; i < LINK_MAX; i++) {
        inode->links[i] = 0;
    }

    nodeinfo->size = 0;
    strcpy(nodeinfo->name, dir->nodename);


    escreverDadosNoSistemaDeArquivos(sb, dirblk, (void *)inode);
    escreverDadosNoSistemaDeArquivos(sb, inode->meta, (void *)nodeinfo);

    // libera a memória alocada
    free(dir);
    free(link);
    free(inode);
    free(nodeinfo);

    return 0;
}

// remove um diretório
int fs_rmdir(struct superblock *sb, const char *dname) {
    struct dir *dir;
    struct link *link;
    struct inode *inode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);
    dir = encontrarInformacoesDoDiretorio(sb, dname);

    if (dir == NULL) {
        free(dir);
        free(inode);
        free(nodeinfo);
        return -1;
    }


    if (dir->nodeblock == 1) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = EBUSY;
        return -1;
    }

    lerDadosDoSistemaDeArquivos(sb, dir->nodeblock, (void *)inode);
    lerDadosDoSistemaDeArquivos(sb, inode->meta, (void *)nodeinfo);

    if (inode->mode != IMDIR) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = ENOTDIR;
        return -1;
    }

    if (nodeinfo->size) {
        free(dir);
        free(inode);
        free(nodeinfo);
        errno = ENOTEMPTY;
        return -1;
    }

    fs_put_block(sb, dir->nodeblock);
    fs_put_block(sb, inode->meta);
    link = encontrarLink(sb, dir->dirnode, dir->nodeblock);
    removerLink(sb, link->inode, link->index);

    // libera a memória alocada
    free(dir);
    free(link);
    free(inode);
    free(nodeinfo);

    return 0;
}
// lista os elementos de um diretório
char *fs_list_dir(struct superblock *sb, const char *dname) {
    char *arq = malloc(NAME_MAX);
    uint64_t elements, size;
    struct dir *dir;
    struct inode *inode = malloc(sb->blksz);
    struct inode *auxinode = malloc(sb->blksz);
    struct nodeinfo *nodeinfo = malloc(sb->blksz);
    struct nodeinfo *auxnodeinfo = malloc(sb->blksz);

    // string de retorno
    strcpy(arq, "");
    dir = encontrarInformacoesDoDiretorio(sb, dname);

    if (dir == NULL) {
        free(dir);
        free(inode);
        free(auxinode);
        free(nodeinfo);
        free(auxnodeinfo);
        return NULL;
    }

    lerDadosDoSistemaDeArquivos(sb, dir->nodeblock, (void *)inode);
    lerDadosDoSistemaDeArquivos(sb, inode->meta, (void *)nodeinfo);


    if (inode->mode != IMDIR) {
        free(dir);
        free(inode);
        free(auxinode);
        free(nodeinfo);
        free(auxnodeinfo);
        errno = ENOTDIR;
        return NULL;
    }

    if (nodeinfo->size == 0) {
        free(dir);
        free(inode);
        free(auxinode);
        free(nodeinfo);
        free(auxnodeinfo);
        return arq;
    }

    elements = 0;
    size = nodeinfo->size;

    // links do diretório
    while (elements < size) {
        for (int i = 0; i < LINK_MAX; i++) {
            if (inode->links[i] != 0) {
                lerDadosDoSistemaDeArquivos(sb, inode->links[i], (void *)auxinode);
                lerDadosDoSistemaDeArquivos(sb, auxinode->meta, (void *)auxnodeinfo);
                strcat(arq, auxnodeinfo->name);
                
                if (auxinode->mode == IMDIR)
                    strcat(arq, "/");

                elements++;

               
                if (elements < size)
                    strcat(arq, " ");
            }
        }
        
        if (inode->next != 0) {
            lerDadosDoSistemaDeArquivos(sb, inode->next, (void *)inode);
        } else {
            break;
        }
    }

    // libera a memória alocada
    free(dir);
    free(inode);
    free(auxinode);
    free(nodeinfo);
    free(auxnodeinfo);

    return arq;
}