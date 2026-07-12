#include <stdio.h>
#include "types.h"
#ifndef FS_H

#define FS_H
#define ROOT_INODE 1
#define BLOCK_SIZE 1024


typedef struct inode inode_t;
typedef struct block block_t;
typedef struct superblock sb_t;
struct superblock{
    uint32_t magic;
    uint32_t block_size;
    uint32_t nblocks;
    uint32_t ninodes;
    uint32_t nlog;
    uint32_t logstart;
    uint32_t inodestart;
    uint32_t bmapstart;
};
#define FSMAGIC 0x10203040
#define NDIRECT 12
#define NINDIRECT (BLOCK_SIZE / sizeof(uint32_t))
#define MAXFILE (NDIRECT + NINDIRECT)
struct dinode {
    uint16_t type;
    uint16_t major;
    uint16_t minor;
    uint16_t nlink;
    uint32_t size;
    uint32_t addrs[NINDIRECT + 1];
};
#endif