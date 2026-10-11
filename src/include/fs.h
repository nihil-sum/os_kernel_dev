#ifndef FS_H
#define FS_H
#include "mm.h"
#include "types.h"
#define FDMAPSZ 8192
// Reserved file descriptor number of stdin stdout stderr
#define STDIN_FN 0
#define STDOUT_FN 1
#define STDERR_FN 2
static u8_t fd_bitmap[FDMAPSZ];
//eg if we wanna set the ord = 51232th fd as used.
// then we will first get b_ord = 51232 >> 3 and this give us which byte
// we would operate on
// then secondly we will get bit_off = 51232 & 7u) this give us which bit
// in the previously got byte we will set as used;
// so the operation to set the bit as used will be as below
// void set_used(int last_used) {
//      int cur = last_used + 1;
//      fd_bitmap[cur >> 3] |= 1u << (cur & 7u); god the codes are insane!
// }
enum file_type {
        NORMAL = 0,
        EXEC   = 1,
        DIR    = 2
};
struct file {
        u16_t          offset;
        int            fd;
        void *         data;
};
struct inode{
        enum file_type type;
        u64_t          size;
        u64_t          dev;
        u64_t          inum;

        int            ref;
        int            valid;
        u16_t          major;
        u16_t          minor;
        u16_t          nlink;
};
struct devsw {

};

int write(struct file* fp,char* s,int size);
int readc(struct file* fp,char* c);
#endif
