#ifndef DS_H
#define DS_H
typedef struct node node_t;
#define list_t node_t*;

struct node{
    void* data;
    node_t* next;
    node_t* prev;
};

#endif