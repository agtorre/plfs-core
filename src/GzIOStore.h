#ifndef _GZ_IOSTORE_H_
#define _GZ_IOSTORE_H_

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <zlib.h>

#include "IOStore.h"
#include "PosixIOStore.h"

/* An implementation of the IOStore for FILE * IO */
/* It inherits from PosixIOStore for everything except the file handle stuff */
class GzIOSHandle: public IOSHandle {
 public:

    GzIOSHandle(string path);

    int Open(int flags, mode_t mode);
    int Fstat(struct stat* buf);
    int Fsync();
    int Ftruncate(off_t length);
    int GetDataBuf(void **bufp, size_t length);
    ssize_t Pread(void* buf, size_t count, off_t offset);
    ssize_t Pwrite(const void* buf, size_t count, off_t offset);
    ssize_t Read(void *buf, size_t count);
    int ReleaseDataBuf(void *buf, size_t length);
    off_t Size();
    ssize_t Write(const void* buf, size_t len);
    
 private:
    int Close();
    int Seek(off_t offset, int whence, off_t *result);
    int fd;
    gzFile gz;
    string path;
};


class GzIOStore: public PosixIOStore {
public:
    IOSHandle *Open(const char *bpath, int flags, mode_t mode, int &ret);
};


#endif
