#include "fs.h"

/* RAM-root construction. Persistent formatting needs a block device first. */
int make_filesystem(struct fs *filesystem) {
        return fs_init(filesystem, 0);
}
