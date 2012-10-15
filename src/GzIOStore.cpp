#include <errno.h>     /* error# ok */

#include "GzIOStore.h"
#include "PosixIOStore.h"
#include "Util.h"

/*
 * IOStore functions that return signed int should return 0 on success
 * and -err on error.   The POSIX API uses 0 for success, -1 for failure
 * with the error code in the global error number variable.   This macro
 * translates POSIX to IOStore.
 *
 * for stdio stuff we assume that EOF is defined to be -1
 */
#if EOF != -1
#error "EOF is not -1"
#endif
#define get_err(X) (X)//(((X) >= 0) ? (X) : -errno)         /* error# ok */
#define get_null_err(X)  (((X) != NULL) ? 0 : -errno)  /* error# ok */

IOSHandle *
GzIOStore::Open(const char *path, int flags, mode_t mode, int &ret) {
    GzIOSHandle *hand = new GzIOSHandle(path);
    ret = hand->Open(flags,mode);
    if (ret == 0) {
        return hand;
    } else {
        delete hand;
        return NULL;
    }
    assert(0);
    return NULL;
}

GzIOSHandle::GzIOSHandle(string newpath) {
    this->path = newpath;
}


int 
GzIOSHandle::Close() {
    int rv;
    rv = gzflush(this->gz,Z_FINISH);
    rv = gzclose(this->gz);
    return(get_err(rv));
}

// helper function to convert flags for POSIX open
// into restrict_mode as used in glib fopen
string
gz_flags_to_restrict_mode(int flags) {
    if (flags & O_RDONLY || flags == O_RDONLY) {
        return "r";
    } else if (flags & O_WRONLY) {
        return "w";
    } else {
        assert (flags & O_RDWR);
        return "r+"; 
    }
    assert(0);
    return "";
}

// returns size or -errno
size_t gzip_uncompressed_size(FILE *fp) {
    // Look at the first two bytes and make sure it's a gzip file
    fseek(fp, 0, SEEK_SET);
    int c1 = fgetc(fp);
    int c2 = fgetc(fp);
    if ( c1 != 0x1f || c2 != 0x8b ) {
        return -EIO;
    }


    // Seek to four bytes from the end of the file
    fseek(fp, -4L, SEEK_END);

    // Array containing the last four bytes
    unsigned char read[4];

    for (int i=0; i<4; ++i ) {
        int charRead = 0;
        if ((charRead = fgetc(fp)) == EOF ) {
            return -EIO;
        } else {
            read[i] = (unsigned char)charRead;
        }
    }

    // Copy the last four bytes into an int.  This could also be done
    // using a union.
    int intval = 0;
    memcpy( &intval, &read, 4 ); 

    if (intval < 0){
        intval = 0;
    }

    return intval;
}

size_t gzip_uncompressed_size(int fd) {
    FILE *fp;
    off_t incoming_off = lseek(fd,0,SEEK_CUR);
    fp = fdopen(dup(fd), "r"); // dup it so fclose won't close it
    if (fp == NULL) {
        return -errno;
    }
    size_t ret = gzip_uncompressed_size(fp);
    fclose(fp);
    lseek(fd,incoming_off,SEEK_SET);
    return ret;
}

// returns size of the uncompressed data or -errno
// only works for files smaller than 4GB....
size_t gzip_uncompressed_size(const char *path) {
    FILE *fp;
    size_t ret;
    if (( fp = fopen( path, "r" )) == NULL ) {
        return -errno;
    }
    ret = gzip_uncompressed_size(fp);
    fclose(fp);
    return ret;
}


int 
GzIOSHandle::Seek(off_t offset, int whence, off_t *result){
    if (whence == SEEK_END) {
        // gzseek doesn't support SEEK_END so we have to
        // figure it out ourselves
        // then seek to end and change whence to SEEK_CUR
        size_t sz = gzip_uncompressed_size(this->fd);
        gzseek(this->gz, sz, SEEK_SET);
        whence = SEEK_CUR;
    }
    z_off_t off = gzseek(this->gz, offset, whence);
    *result = (off_t)off;
    return 0;
}

int 
GzIOSHandle::Open(int flags, mode_t mode) {
    int rv;
    int fd = open(path.c_str(),flags,mode);
    if (fd < 0) {
        return(get_err(fd));
    }

    // the open was successful, turn into FILE *
    // currently using fdopen following posix open
    // but might be better performance to just fopen
    // and then chmod
    string restrict_mode = gz_flags_to_restrict_mode(flags);
    this->fd = fd;
    this->gz = gzdopen(fd,restrict_mode.c_str());
    rv = get_null_err(this->gz);
    if (rv < 0) {
        close(fd);    // cleanup
    }

    return(rv);
}

int 
GzIOSHandle::Fstat(struct stat* buf) {
    int rv;
    rv = fstat(this->fd, buf);
    return(get_err(rv));
};

int 
GzIOSHandle::Fsync() {
    int rv;
    rv = gzflush(this->gz,Z_FULL_FLUSH); 
    if (rv == Z_OK) {
        rv = 0;
    }
    return(get_err(rv));
};

int 
GzIOSHandle::Ftruncate(off_t length) {
    int rv;
    this->Fsync();
    rv = ftruncate(this->fd, length);
    return(get_err(rv));
};

int
GzIOSHandle::GetDataBuf(void **bufp, size_t length) {
    int ret;
    ssize_t rv;
    *bufp = malloc(length);
    if (*bufp == NULL) {
        ret = -errno;
    } else {
        off_t result;
        this->Seek(0, SEEK_SET, &result);
        ret = this->Read(*bufp,length);
        if (ret > 0) {
            ret = 0;
        }
    }
    rv = get_err(ret);
    return(rv);
}

ssize_t 
GzIOSHandle::Pread(void* buf, size_t count, off_t offset) {
    ssize_t rv;
    int ret;
    /* XXX: we need some mutex locking here for concurrent access? */
    off_t result;
    ret = this->Seek(offset,SEEK_SET,&result);
    rv = get_err(ret);
    if (rv == 0) {
        ret = this->Read(buf, count);
        if (ret == 0 ) {
            ret = -1;
        }
        rv = get_err(ret);
    }
    return(rv);
};

ssize_t 
GzIOSHandle::Pwrite(const void* buf, size_t count, off_t offset) {
    ssize_t rv;
    int ret;
    /* XXX: we need some mutex locking here for concurrent access? */
    off_t result;
    ret = this->Seek(offset,SEEK_SET,&result);
    rv = get_err(ret);
    if (rv == 0) {
        ret = this->Write(buf, count);
        if (ret == 0 ){
            ret = -1;
        }
        rv = get_err(ret);
    }
    return(rv);
};

ssize_t 
GzIOSHandle::Read(void *buf, size_t count) {
    ssize_t rv;
    rv = gzread(this->gz, buf,count);
    if (rv == 0 ){
        rv = get_err(-1);
    }
    return(rv);
};

int 
GzIOSHandle::ReleaseDataBuf(void *addr, size_t length)
{
    int rv;
    rv = 0;
    free(addr);
    return(get_err(rv));
}

off_t 
GzIOSHandle::Size() {
    return gzip_uncompressed_size(this->fd);
}

ssize_t 
GzIOSHandle::Write(const void* buf, size_t len) {
    ssize_t rv;
    rv = gzwrite(this->gz,buf,len);
    if (rv == 0){
        rv = get_err(-1);
    }
    return(rv);
};


