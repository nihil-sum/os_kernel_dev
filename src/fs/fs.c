#include "fs.h"
#include "mm.h"
// this is a basic write function which accepts
// the pointer to the file object and the string to write into
// the file and the size waiting to write.
// the ret val will be the size writing into the file.
int write(struct file* fp,char* s,int size) {
        if (!fp || size < 0 || !fp->data || !s) return -1;
        if (fp->offset > PAGE_SIZE) return -1;
        int cp = 0;
        while(fp->offset < PAGE_SIZE && cp < size) {
                *(char*)(fp->data + fp->offset++) = s[cp++];
        }
        return cp;
}
// this is a basic byte by byte read function.
int readc(struct file* fp,char* c) {
        if(!fp || fp->offset <= 0 || !fp->data || fp->offset >= PAGE_SIZE) return -1;
        if(!c) return -1;
        *c = *((char*)fp->data + fp->offset++);
        return 1;
}
