
#ifndef BIO_H
#define BIO_H
#include "types.h"

err_t bread(buf_t* buffer, uint32_t blockno);
err_t bwrite(buf_t* buffer, uint32_t blockno);
err_t brelse(buf_t* buffer);


#endif