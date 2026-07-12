#include "buf.h"
#include "types.h"
#include <stdio.h>
#include <string.h>
list_t bufinit(void){
    list_t head = malloc(sizeof(node_t));
    return head;
}
err_t flush_buf(buf_t* buffer){
    // Implement the logic to flush the buffer to disk
    // This is a placeholder implementation
    if (buffer == NULL) {
        err_t err = {1, "Buffer is NULL"};
        return err;
    }
    
    memset(buffer->data, 0, BUF_SIZE); // Clear the buffer data
    
    err_t success = {0, "Buffer flushed successfully"};
    return success;
}
