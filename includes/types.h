#ifndef TYPES_H
#define TYPES_H
#include <stdint.h>
#define BUF_SIZE 4096
typedef struct node node_t,*list_t;

struct node{
    void* data;
    node_t* next;
    node_t* prev;
};
typedef struct error err_t;
struct error{
    int is_error;
    char * msg;
};
typedef struct buffer buf_t;
struct buffer{
    void * data;
    int buf_no;
    size_t buf_size;
};

#endif