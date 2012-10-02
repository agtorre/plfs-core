#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <errno.h>
#include "COPYRIGHT.h"
#include <string>
#include <assert.h>
#include <fstream>
#include <iostream>
#include <sys/dir.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/param.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <time.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <map>
#include <stdio.h>
#include <stdarg.h>
using namespace std;

#include "FileOp.h"
#include "Util.h"
#include "LogMessage.h"
#include "mlogfacs.h"
#include "Container.h"
#include "mlog_oss.h"

#ifdef HAVE_SYS_FSUID_H
#include <sys/fsuid.h>
#endif

#define SLOW_UTIL   2

#define O_CONCURRENT_WRITE                         020000000000


// here is some code to enable transparent gzipping of file IO
enum IOType {
    POSIX_IO,
    GNU_IO,
    GZIP_IO,
};

IOType iotype = GZIP_IO;    // which IO type to use
pthread_mutex_t handle_map_mux = PTHREAD_MUTEX_INITIALIZER;
int available_handle = 0;
HASH_MAP<int, gzFile> gz_handle_map;

// TODO.  Some functions in here return -errno.  Probably none of them
// should

// shoot.  I just realized.  All this close timing stuff is not thread safe.
// maybe we should add a mutex in addBytes and addTime.
// might slow things down but this is supposed to just be for debugging...

#ifndef UTIL_COLLECT_TIMES
off_t total_ops = 0;
#define ENTER_UTIL int ret = 0; total_ops++;
#define ENTER_IO   ssize_t ret = 0;
#define EXIT_IO    return ret;
#define EXIT_UTIL  return ret;
#define ENTER_MUX  ENTER_UTIL;
#define ENTER_PATH ENTER_UTIL;
#else
#define DEBUG_ENTER /* mlog(UT_DAPI, "Enter %s", __FUNCTION__ );*/
#define DEBUG_EXIT  LogMessage lm1;                             \
                        mss::mlog_oss oss(UT_DAPI);             \
                        oss << "Util::" << setw(13) << __FUNCTION__; \
                        if (path) oss << " on " << path << " ";     \
                        oss << setw(7) << " ret=" << setprecision(0) << ret    \
                            << " " << setprecision(4) << fixed      \
                            << end-begin << endl; \
                        lm1 << oss.str();                           \
                        lm1.flush();                                \
                        mlog(UT_DAPI, "%s", oss.str().c_str());

#define ENTER_MUX   LogMessage lm2;                             \
                        lm2 << "Util::" << setw(13) << __FUNCTION__ \
                            << endl;                                \
                        lm2.flush();                            \
                        ENTER_UTIL;

#define ENTER_PATH   int ret = 0;                                \
                         LogMessage lm4;                             \
                         lm4 << "Util::" << setw(13) << __FUNCTION__ \
                             << " on "   << path << endl;            \
                         lm4.flush();                            \
                         ENTER_SHARED;

#define ENTER_SHARED double begin,end;  \
                        DEBUG_ENTER;        \
                        begin = getTime();

#define ENTER_UTIL  const char *path = NULL; \
                        int ret = 0;       \
                        ENTER_SHARED;

#define ENTER_IO    const char *path = NULL; \
                        ssize_t ret = 0;    \
                        ENTER_SHARED;

#define EXIT_SHARED DEBUG_EXIT;                                 \
                        addTime( __FUNCTION__, end - begin, (ret<0) );       \
                        if ( end - begin > SLOW_UTIL ) {            \
                            LogMessage lm3;                         \
                            lm3 << "WTF: " << __FUNCTION__          \
                                << " took " << end-begin << " secs" \
                                << endl;                            \
                            lm3.flush();                            \
                        }                                           \
                        return ret;

#define EXIT_IO     end   = getTime();              \
                        addBytes( __FUNCTION__, size ); \
                        EXIT_SHARED;

// hmm, want to pass an arg to this macro but it didn't work...
#define EXIT_UTIL   end   = getTime();                          \
                        EXIT_SHARED;
#endif

// function that tokenizes a string into a set of strings based on set of delims
vector<string> &Util::tokenize(const string& str,const string& delimiters,
                               vector<string> &tokens)
{
    // skip delimiters at beginning.
    string::size_type lastPos = str.find_first_not_of(delimiters, 0);
    // find first "non-delimiter".
    string::size_type pos = str.find_first_of(delimiters, lastPos);
    while (string::npos != pos || string::npos != lastPos) {
        // found a token, add it to the vector.
        tokens.push_back(str.substr(lastPos, pos - lastPos));
        // skip delimiters.  Note the "not_of"
        lastPos = str.find_first_not_of(delimiters, pos);
        // find next "non-delimiter"
        pos = str.find_first_of(delimiters, lastPos);
    }
    return tokens;
}

void
Util::SeriousError( string msg, pid_t pid )
{
    string filename = getenv("HOME");
    ostringstream oss;
    oss << getenv("HOME") << "/plfs.error." << hostname() << "." << pid;
    FILE *debugfile = fopen( oss.str().c_str(), "a" );
    if ( ! debugfile ) {
        cerr << "PLFS ERROR: Couldn't open " << oss.str()
             << " for serious error: " << msg << endl;
    } else {
        fprintf(debugfile,"%s\n",msg.c_str());
        fclose(debugfile);
    }
}

void
Util::OpenError(const char *file, const char *func, int line, int Err, pid_t p)
{
    ostringstream oss;
    oss << "open() error seen at " << file << ":" << func << ":" << line << ": "
        << strerror(Err);
    //SeriousError(oss.str(), p);
}

// initialize static variables
HASH_MAP<string, double> utimers;
HASH_MAP<string, off_t>  kbytes;
HASH_MAP<string, off_t>  counters;
HASH_MAP<string, off_t>  errors;

string Util::toString( )
{
    ostringstream oss;
    string output;
    off_t  total_ops  = 0;
    off_t  total_errs = 0;
    double total_time = 0.0;
    HASH_MAP<string,double>::iterator itr;
    HASH_MAP<string,off_t> ::iterator kitr;
    HASH_MAP<string,off_t> ::iterator count;
    HASH_MAP<string,off_t> ::iterator err;
    for( itr = utimers.begin(); itr != utimers.end(); itr++ ) {
        count  = counters.find( itr->first );
        err = errors.find( itr->first );
        output += timeToString( itr, err, count, &total_errs,
                                &total_ops, &total_time );
        if ( ( kitr = kbytes.find(itr->first) ) != kbytes.end() ) {
            output += bandwidthToString( itr, kitr );
        }
        output += "\n";
    }
    oss << "Util Total Ops " << total_ops << " Errs "
        << total_errs << " in "
        << std::setprecision(2) << std::fixed << total_time << "s\n";
    output += oss.str();
    return output;
}

string Util::bandwidthToString( HASH_MAP<string,double>::iterator itr,
                                HASH_MAP<string,off_t> ::iterator kitr )
{
    off_t kbs   = kitr->second;
    double time = itr->second;
    double bw   = (kbs/time) / 1024;
    ostringstream oss;
    oss << ", " << setw(6) << kbs << "KBs "
        << std::setprecision(2) << std::fixed << setw(8) << bw << "MB/s";
    return oss.str();
}

string Util::timeToString( HASH_MAP<string,double>::iterator itr,
                           HASH_MAP<string,off_t>::iterator eitr,
                           HASH_MAP<string,off_t>::iterator citr,
                           off_t *total_errs,
                           off_t *total_ops,
                           double *total_time )
{
    double value    = itr->second;
    off_t  count    = citr->second;
    off_t  errs     = eitr->second;
    double avg      = (double) count / value;
    ostringstream oss;
    *total_errs += errs;
    *total_ops  += count;
    *total_time += value;
    oss << setw(12) << itr->first << ": " << setw(8) << count << " ops, "
        << setw(8) << errs << " errs, "
        << std::setprecision(2)
        << std::fixed
        << setw(8)
        << value
        << "s time, "
        << setw(8)
        << avg
        << "ops/s";
    return oss.str();
}

void Util::addBytes( string function, size_t size )
{
    HASH_MAP<string,off_t>::iterator itr;
    itr = kbytes.find( function );
    if ( itr == kbytes.end( ) ) {
        kbytes[function] = (size / 1024);
    } else {
        kbytes[function] += (size / 1024);
    }
}


/*

   // we don't need this code anymore since we still use posix open and then
   // use gzdopen and can use fdopen if we want glib

// small utility to allow swapping in gzip or FILE IO
// will be replaced by iostore but this is simple way to test with old branches
// returns a positive fd or -errno
int
Util::add_file_handle(void *handle){

    // use to make sure it got inserted
    pair<map<char,void *>::iterator,bool> ret;

    // use a counter for the hopefully impossible case in which we have no
    // available slots available to store a new gzfile
    int start_value = available_handle;
    bool inserted = false;
    pthread_mutex_lock( &handle_map_mux );
    do {
        available_handle++;
        if (available_handle < 3){ // cannot be negative or any of the STD's
            available_handle = 3;
        }
        handle_map.insert(pair<int,void*>(available_handle, handle));
        if (ret.second==true) {
            mlog(UT_DAPI, "%s found fd slot %d",__FUNCTION__, available_handle);
            inserted = true;
            break;
        }
    } while (start_value != available_handle );
    pthread_mutex_unlock( &handle_map_mux );

    // boy, I hope this never happens
    if ( ! inserted ) {
        mlog(UT_CRIT, "WTF: %s failed with no available fd slots",__FUNCTION__);
        return -EMFILE;
    }

    // success
    return available_handle;
}

int
Util::remove_file_handle(int fd){
    pthread_mutex_lock( &handle_map_mux );
    handle_map.erase(fd);
    pthread_mutex_unlock( &handle_map_mux );
    return 0;
}
*/

// just reads through a directory and returns all descendants
// useful for gathering the contents of a container
int
Util::traverseDirectoryTree(const char *path, vector<string> &files,
                            vector<string> &dirs, vector<string> &links)
{
    ENTER_PATH;
    mlog(UT_DAPI, "%s on %s", __FUNCTION__, path);
    map<string,unsigned char> entries;
    map<string,unsigned char>::iterator itr;
    ReaddirOp rop(&entries,NULL,true,true);
    string resolved;
    ret = rop.op(path,DT_DIR);
    if (ret==-ENOENT) {
        return 0;    // no shadow or canonical on this backend: np.
    }
    if (ret!=0) {
        return ret;    // some other error is a problem
    }
    dirs.push_back(path); // save the top dir
    for(itr = entries.begin(); itr != entries.end() && ret==0; itr++) {
        if (itr->second == DT_DIR) {
            ret = traverseDirectoryTree(itr->first.c_str(),files,dirs,links);
        } else if (itr->second == DT_LNK) {
            links.push_back(itr->first);
            ret = Container::resolveMetalink(itr->first, resolved);
            if (ret == 0) {
                ret = traverseDirectoryTree(resolved.c_str(),files,dirs,links);
            }
        } else {
            files.push_back(itr->first);
        }
    }
    EXIT_UTIL;
}

pthread_mutex_t time_mux;
void Util::addTime( string function, double elapsed, bool error )
{
    HASH_MAP<string,double>::iterator itr;
    HASH_MAP<string,off_t>::iterator two;
    HASH_MAP<string,off_t>::iterator three;
    // plfs is hanging in here for some reason
    // is it a concurrency problem?
    // idk.  be safe and put it in a mux.  testing
    // or rrp3 is where I saw the problem and
    // adding this lock didn't slow it down
    // also, if you're worried, just turn off
    // both util timing (-DUTIL) and -DNPLFS_TIMES
    pthread_mutex_lock( &time_mux );
    itr   = utimers.find( function );
    two   = counters.find( function );
    three = errors.find( function );
    if ( itr == utimers.end( ) ) {
        utimers[function] = elapsed;
        counters[function] = 1;
        if ( error ) {
            errors[function] = 1;
        }
    } else {
        utimers[function] += elapsed;
        counters[function] ++;
        if ( error ) {
            errors[function] ++;
        }
    }
    pthread_mutex_unlock( &time_mux );
}

int Util::Utime( const char *path, const struct utimbuf *buf )
{
    ENTER_PATH;
    ret = utime( path, buf );
    EXIT_UTIL;
}

int Util::Unlink( const char *path )
{
    ENTER_PATH;
    ret = unlink( path );
    EXIT_UTIL;
}


int Util::Access( const char *path, int mode )
{
    ENTER_PATH;
    ret = access( path, mode );
    EXIT_UTIL;
}

int Util::Mknod( const char *path, mode_t mode, dev_t dev )
{
    ENTER_PATH;
    ret = mknod( path, mode, dev );
    EXIT_UTIL;
}

int Util::Truncate( const char *path, off_t length )
{
    ENTER_PATH;
    ret = truncate( path, length );
    EXIT_UTIL;
}


int Util::MutexLock(  pthread_mutex_t *mux , const char *where )
{
    ENTER_MUX;
    mss::mlog_oss os(UT_DAPI), os2(UT_DAPI);
    os << "Locking mutex " << mux << " from " << where;
    mlog(UT_DAPI, "%s", os.str().c_str() );
    pthread_mutex_lock( mux );
    os2 << "Locked mutex " << mux << " from " << where;
    mlog(UT_DAPI, "%s", os2.str().c_str() );
    EXIT_UTIL;
}

int Util::MutexUnlock( pthread_mutex_t *mux, const char *where )
{
    ENTER_MUX;
    mss::mlog_oss os(UT_DAPI);
    os << "Unlocking mutex " << mux << " from " << where;
    mlog(UT_DAPI, "%s", os.str().c_str() );
    pthread_mutex_unlock( mux );
    EXIT_UTIL;
}

ssize_t Util::Pread( int fd, void *buf, size_t size, off_t off )
{
    ENTER_IO;
    switch(iotype) {
        case POSIX_IO:
            ret = pread( fd, buf, size, off );
            break;
        case GZIP_IO:
            {
                // just seek it and use the existing function
                gzFile gz = (gzFile)gz_handle_map[fd];
                z_off_t offset = gzseek(gz, off, SEEK_SET);
                if (offset == off) {
                    ret = Read(fd,buf,size); 
                } else {
                    ret = (errno==0?-EIO:-errno);
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_IO;
}

ssize_t Util::Pwrite( int fd, const void *buf, size_t size, off_t off )
{
    ENTER_IO;
    switch(iotype) {
        case POSIX_IO:
            ret = pwrite( fd, buf, size, off );
            break;
        case GZIP_IO:
            {
                // just seek it and use the existing function
                gzFile gz = (gzFile)gz_handle_map[fd];
                z_off_t offset = gzseek(gz, off, SEEK_SET);
                if (offset == off) {
                    ret = Write(fd,buf,size); 
                } else {
                    ret = (errno==0?-EIO:-errno);
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_IO;
}

int Util::Rmdir( const char *path )
{
    ENTER_PATH;
    ret = rmdir( path );
    EXIT_UTIL;
}

int Util::Lstat( const char *path, struct stat *st )
{
    ENTER_PATH;
    ret = lstat( path, st );
    EXIT_UTIL;
}

int Util::Rename( const char *path, const char *to )
{
    ENTER_PATH;
    ret = rename( path, to );
    EXIT_UTIL;
}

// Use most popular used read+write to copy file,
// as mmap/sendfile/splice may fail on some system.
int Util::CopyFile( const char *path, const char *to )
{
    ENTER_PATH;
    int fd_from, fd_to, buf_size;
    ssize_t read_len, write_len, copy_len;
    char *buf = NULL, *ptr;
    struct stat sbuf;
    mode_t stored_mode;
    ret = Lstat(path, &sbuf);
    if (ret) {
        goto out;
    }
    ret = -1;
    if (S_ISLNK(sbuf.st_mode)) { // copy a symbolic link.
        buf = (char *)calloc(1,PATH_MAX);
        if (!buf) {
            goto out;
        }
        read_len = Readlink(path, buf, PATH_MAX);
        if (read_len < 0) {
            goto out;
        }
        buf[read_len] = 0;
        ret = Symlink(buf, to);
        goto out;
    }
    fd_from = Open(path, O_RDONLY);
    if (fd_from<0) {
        goto out;
    }
    stored_mode = umask(0);
    fd_to = Open(to, O_WRONLY | O_CREAT, sbuf.st_mode);
    umask(stored_mode);
    if (fd_to<0) {
        Close(fd_from);
        goto out;
    }
    if (!sbuf.st_size) {
        ret = 0;
        goto done;
    }
    buf_size = sbuf.st_blksize;
    buf = (char *)calloc(1,buf_size);
    if (!buf) {
        goto done;
    }
    copy_len = 0;
    while ((read_len = Read(fd_from, buf, buf_size)) != 0) {
        if ((read_len==-1)&&(errno!=EINTR)) {
            break;
        } else if (read_len>0) {
            ptr = buf;
            while ((write_len = Write(fd_to, ptr, read_len)) != 0) {
                if ((write_len==-1)&&(errno!=EINTR)) {
                    goto done;
                } else if (write_len==read_len) {
                    copy_len += write_len;
                    break;
                } else if(write_len>0) {
                    ptr += write_len;
                    read_len -= write_len;
                    copy_len += write_len;
                }
            }
        }
    }
    if (copy_len==sbuf.st_size) {
        ret = 0;
    }
    if (ret)
        mlog(UT_DCOMMON, "Util::CopyFile, copy from %s to %s, ret: %d, %s",
             path, to, ret, strerror(errno));
done:
    Close(fd_from);
    Close(fd_to);
    if (ret) {
        Unlink(to);    // revert our change, delete the file created.
    }
out:
    if (buf) {
        free(buf);
    }
    EXIT_UTIL;
}

ssize_t Util::Readlink(const char *link, char *buf, size_t bufsize)
{
    ENTER_IO;
    ret = readlink(link,buf,bufsize);
    EXIT_UTIL;
}

int Util::Link( const char *path, const char *to )
{
    ENTER_PATH;
    ret = link( path, to );
    EXIT_UTIL;
}

int Util::Symlink( const char *path, const char *to )
{
    ENTER_PATH;
    ret = symlink( path, to );
    EXIT_UTIL;
}

ssize_t Util::Read( int fd, void *buf, size_t size)
{
    ENTER_IO;
    switch(iotype) {
        case POSIX_IO:
            ret = read( fd, buf, size );
            break;
        case GZIP_IO:
            {
                gzFile gz = (gzFile)gz_handle_map[fd];
                ret = gzread(gz,buf,size);
                if (ret < 0) {
                    ret = (errno==0?-EIO:-errno);
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_IO;
}

ssize_t Util::Write( int fd, const void *buf, size_t size)
{
    ENTER_IO;
    switch(iotype) {
        case POSIX_IO:
            ret = write( fd, buf, size );
            break;
        case GZIP_IO:
            {
                gzFile gz = (gzFile)gz_handle_map[fd];
                ret = gzwrite(gz,buf,size);
                if (ret < 0) {
                    ret = (errno==0?-EIO:-errno);
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_IO;
}

int Util::Close( int fd )
{
    ENTER_UTIL;
    switch(iotype) {
        case POSIX_IO:
            ret = close(fd);
            break;
        case GZIP_IO:
            {
                gzFile gz = (gzFile)gz_handle_map[fd];
                ret = gzclose(gz);
                gz_handle_map.erase(fd);
                ret = (ret == Z_OK?0:errno==0?-EIO:-errno);
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_UTIL;
}

int Util::Creat( const char *path, mode_t mode )
{
    ENTER_PATH;
    ret = creat( path, mode );
    if ( ret > 0 ) {
        ret = close( ret );
    } else {
        ret = -errno;
    }
    EXIT_UTIL;
}

int Util::Statvfs( const char *path, struct statvfs *stbuf )
{
    ENTER_PATH;
    ret = statvfs(path,stbuf);
    EXIT_UTIL;
}

char *Util::Strdup(const char *s1)
{
    return strdup(s1);
}


// returns 0 if success, 1 if end of dir, -errno if error
int Util::Readdir(DIR *dir, struct dirent **de)
{
    ENTER_UTIL;
    errno = 0;
    *de = NULL;
    *de = readdir(dir);
    if (*de) {
        ret = 0;
    } else if (errno == 0) {
        ret = 1;
    } else {
        ret = -errno;
    }
    mlog(UT_DCOMMON, "readdir returned %p (ret %d, errno %d)", *de, ret, errno);
    EXIT_UTIL;
}

// returns 0 or -errno
int Util::Opendir( const char *path, DIR **dp )
{
    ENTER_PATH;
    *dp = opendir( path );
    ret = ( *dp == NULL ? -errno : 0 );
    EXIT_UTIL;
}

int Util::Closedir( DIR *dp )
{
    ENTER_UTIL;
    ret = closedir( dp );
    EXIT_UTIL;
}

int Util::Munmap(void *addr,size_t len)
{
    ENTER_UTIL;
    switch(iotype) {
        case POSIX_IO:
            ret = munmap(addr,len);
            break;
        case GZIP_IO:
            {
                free(addr);
                ret = 0;
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_UTIL;
}

int Util::Mmap( size_t len, int fd, void **retaddr)
{
    ENTER_UTIL;
    switch(iotype) {
        case POSIX_IO:
            {
                int prot  = PROT_READ;
                int flags = MAP_PRIVATE|MAP_NOCACHE;
                *retaddr = mmap( NULL, len, prot, flags, fd, 0 );
                ret = (*retaddr == (void *)NULL || *retaddr ==(void *)-1 ?-1:0);
            }
            break;
        case GZIP_IO:
            {
                // fake mmap by mallocing a buf and reading into it
                mlog(UT_DAPI, "Faking mmap for length %ld", len);
                *retaddr = malloc(len);
                if (*retaddr == NULL) {
                    ret = -errno;
                } else {
                    ret = Read(fd,*retaddr,len);
                    if (ret > 0) {
                        ret = 0;
                    }
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_UTIL;
}

// returns size or -errno
size_t gzip_uncompressed_size(FILE *fp) {
    // Look at the first two bytes and make sure it's a gzip file
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

    mlog(UT_DAPI, "Found size of gzip file: %ld", (long int)intval); 
    return intval;
}

size_t gzip_uncompressed_size(int fd) {
    FILE *fp;
    fp = fdopen(dup(fd), "r"); // dup it so fclose won't close it
    if (fp == NULL) {
        return -errno;
    }
    size_t ret = gzip_uncompressed_size(fp);
    fclose(fp);
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

int Util::Lseek( int fd, off_t offset, int whence, off_t *result )
{
    ENTER_UTIL;
    switch(iotype) {
        case POSIX_IO:
            *result = lseek( fd, offset, whence );
            break;
        case GZIP_IO:
            {
                gzFile gz = (gzFile)gz_handle_map[fd];
                if (whence == SEEK_END) {
                    // gzseek doesn't support SEEK_END so we have to
                    // figure it out ourselves
                    // then seek to end and change whence to SEEK_CUR
                    size_t sz = gzip_uncompressed_size(fd);          
                    gzseek(gz, sz, SEEK_SET); 
                    whence = SEEK_CUR; 
                }
                z_off_t off = gzseek(gz, offset, whence);
                mlog(UT_DAPI, "gzseek got %ld", (long int)off);
                *result = (off_t)off;
            }
            break;
        case GNU_IO:
        default:
            *result = -ENOSYS;
            break;
    }
    ret = (int)*result;
    EXIT_UTIL;
}

// helper function to convert flags for POSIX open
// into restrict_mode as used in glib fopen
string
flags_to_mode(int flags) {
    if (flags & O_RDONLY || flags == O_RDONLY) {
        // tougher to check for O_RDONLY since it is 0
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

int Util::Open( const char *path, int flags )
{
    ENTER_PATH;
    // first do the regular open
    int fd = open( path, flags );

    switch(iotype) {
        case POSIX_IO:
            ret = fd;
            break;
        case GZIP_IO:
            {
                string mode = flags_to_mode(flags);
                gzFile gz = gzdopen(fd, mode.c_str());
                if (gz == Z_NULL) {
                    ret = -EIO;
                } else {
                    gz_handle_map[fd] = gz;
                    ret = fd; 
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_UTIL;
}

int Util::Open( const char *path, int flags, mode_t mode )
{
    ENTER_PATH;

    // first do the regular open
    int fd = open( path, flags, mode );

    switch(iotype) {
        case POSIX_IO:
            ret = fd;
            break;
        case GZIP_IO:
            {
                string mode = flags_to_mode(flags);
                gzFile gz = gzdopen(fd, mode.c_str());
                if (gz == Z_NULL) {
                    ret = -EIO;
                } else {
                    gz_handle_map[fd] = (void*)gz;
                    ret = fd; 
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_UTIL;
}

bool Util::exists( const char *path )
{
    ENTER_PATH;
    bool exists = false;
    struct stat buf;
    if (Util::Lstat(path, &buf) == 0) {
        exists = true;
    }
    ret = exists;
    EXIT_UTIL;
}

bool Util::isDirectory( struct stat *buf )
{
    return (S_ISDIR(buf->st_mode) && !S_ISLNK(buf->st_mode));
}

bool Util::isDirectory( const char *path )
{
    ENTER_PATH;
    bool exists = false;
    struct stat buf;
    if ( Util::Lstat( path, &buf ) == 0 ) {
        exists = isDirectory( &buf );
    }
    ret = exists;
    EXIT_UTIL;
}

int Util::Chown( const char *path, uid_t uid, gid_t gid )
{
    ENTER_PATH;
    ret = chown( path, uid, gid );
    EXIT_UTIL;
}

int Util::Lchown( const char *path, uid_t uid, gid_t gid )
{
    ENTER_PATH;
    ret = lchown( path, uid, gid );
    EXIT_UTIL;
}

int Util::Chmod( const char *path, int flags )
{
    ENTER_PATH;
    ret = chmod( path, flags );
    EXIT_UTIL;
}

int Util::Mkdir( const char *path, mode_t mode )
{
    ENTER_PATH;
    ret = mkdir( path, mode );
    EXIT_UTIL;
}

int Util::Filesize(const char *path)
{
    ENTER_PATH;
    struct stat stbuf;
    ret = Stat(path,&stbuf);
    if (ret==0) {
        ret = (int)stbuf.st_size;
    }
    EXIT_UTIL;
}

int Util::Fsync( int fd)
{
    ENTER_UTIL;
    switch(iotype) {
        case POSIX_IO:
            ret = fsync( fd );
            break;
        case GZIP_IO:
            {
                gzFile gz = (gzFile)gz_handle_map[fd];
                ret = gzflush(gz,Z_FINISH);
                if (ret == Z_OK) {
                    ret = 0;
                } else {
                    ret = (errno==0?-EIO:-errno);
                }
            }
            break;
        case GNU_IO:
        default:
            ret = -ENOSYS;
            break;
    }
    EXIT_UTIL;
}

double Util::getTime( )
{
    // shoot this seems to be solaris only
    // how does MPI_Wtime() work?
    //return 1.0e-9 * gethrtime();
    struct timeval time;
    if ( gettimeofday( &time, NULL ) != 0 ) {
        mlog(UT_CRIT, "WTF: %s failed: %s",
             __FUNCTION__, strerror(errno));
    }
    return (double)time.tv_sec + time.tv_usec/1.e6;
}

// returns n or returns -1
ssize_t Util::Writen( int fd, const void *vptr, size_t n )
{
    ENTER_UTIL;
    size_t      nleft;
    ssize_t     nwritten;
    const char  *ptr;
    ptr = (const char *)vptr;
    nleft = n;
    ret   = n;
    while (nleft > 0) {
        if ( (nwritten = Util::Write(fd, ptr, nleft)) <= 0) {
            if (errno == EINTR) {
                nwritten = 0;    /* and call write() again */
            } else {
                ret = -1;           /* error */
                break;
            }
        }
        nleft -= nwritten;
        ptr   += nwritten;
    }
    EXIT_UTIL;
}

string Util::openFlagsToString( int flags )
{
    string fstr;
    if ( flags & O_WRONLY ) {
        fstr += "w";
    }
    if ( flags & O_RDWR ) {
        fstr += "rw";
    }
    if ( flags & O_RDONLY ) {
        fstr += "r";
    }
    if ( flags & O_CREAT ) {
        fstr += "c";
    }
    if ( flags & O_EXCL ) {
        fstr += "e";
    }
    if ( flags & O_TRUNC ) {
        fstr += "t";
    }
    if ( flags & O_APPEND ) {
        fstr += "a";
    }
    if ( flags & O_NONBLOCK || flags & O_NDELAY ) {
        fstr += "n";
    }
    if ( flags & O_SYNC ) {
        fstr += "s";
    }
    if ( flags & O_DIRECTORY ) {
        fstr += "D";
    }
    if ( flags & O_NOFOLLOW ) {
        fstr += "N";
    }
#ifndef __APPLE__
    if ( flags & O_LARGEFILE ) {
        fstr += "l";
    }
    if ( flags & O_DIRECT ) {
        fstr += "d";
    }
    if ( flags & O_NOATIME ) {
        fstr += "A";
    }
#else
    if ( flags & O_SHLOCK ) {
        fstr += "S";
    }
    if ( flags & O_EXLOCK ) {
        fstr += "x";
    }
    if ( flags & O_SYMLINK ) {
        fstr += "L";
    }
#endif
    /*
    if ( flags & O_ATOMICLOOKUP ) {
        fstr += "d";
    }
    */
    // what the hell is flags: 0x8000
    if ( flags & 0x8000 ) {
        fstr += "8";
    }
    if ( flags & O_CONCURRENT_WRITE ) {
        fstr += "cw";
    }
    if ( flags & O_NOCTTY ) {
        fstr += "c";
    }
    if ( O_RDONLY == 0 ) { // this is O_RDONLY I think
        // in the header, O_RDONLY is 00
        int rdonlymask = 0x0002;
        if ( (rdonlymask & flags) == 0 ) {
            fstr += "r";
        }
    }
    ostringstream oss;
    oss << fstr << " (" << flags << ")";
    fstr = oss.str();
    return oss.str();
}

/*
// replaces a "%h" in a path with the hostname
string Util::expandPath( string path, string hostname ) {
    size_t found = path.find( "%h" );
    if ( found != string::npos ) {
        path.replace( found, strlen("%h"), hostname );
    }
    return path;
}
*/

uid_t Util::Getuid()
{
    ENTER_UTIL;
#ifndef __APPLE__
    ret = getuid();
#endif
    EXIT_UTIL;
}

gid_t Util::Getgid()
{
    ENTER_UTIL;
#ifndef __APPLE__
    ret = getgid();
#endif
    EXIT_UTIL;
}

int Util::Setfsgid( gid_t g )
{
    ENTER_UTIL;
#ifndef __APPLE__
    errno = 0;
    ret = setfsgid( g );
    mlog(UT_DCOMMON, "Set gid %d: %s", g, strerror(errno) );
#endif
    EXIT_UTIL;
}

int Util::Setfsuid( uid_t u )
{
    ENTER_UTIL;
#ifndef __APPLE__
    errno = 0;
    ret = setfsuid( u );
    mlog(UT_DCOMMON, "Set uid %d: %s", u, strerror(errno) );
#endif
    EXIT_UTIL;
}

// a utility for turning return values into 0 or -ERRNO
int Util::retValue( int res )
{
    return (res == 0 ? 0 : -errno);
}

char *Util::hostname()
{
    static bool init = false;
    static char hname[128];
    if ( !init && gethostname(hname, sizeof(hname)) < 0) {
        return NULL;
    }
    init = true;
    return hname;
}

int Util::Stat(const char *path, struct stat *file_info)
{
    ENTER_PATH;
    // first stat the file
    ret = stat( path , file_info );

    // if we are using gzip files, we need to get uncompressed filesize 
    if (iotype == GZIP_IO) {
        file_info->st_size = gzip_uncompressed_size(path);
        ret = (file_info->st_size > 0 ? 0 : file_info->st_size);
    }
    EXIT_UTIL;
}

int Util::Fstat(int fd, struct stat *file_info)
{
    ENTER_UTIL;
    // just use the fd, but if GZIP_IO or GNU_IO, flush first
    if (iotype == GZIP_IO || iotype == GNU_IO) {
        Fsync(fd);
    }
    ret = fstat(fd, file_info);

    // if we are using gzip files, we need to get uncompressed filesize 
    if (iotype == GZIP_IO) {
        file_info->st_size = gzip_uncompressed_size(fd);
        ret = (file_info->st_size > 0 ? 0 : file_info->st_size);
    }
    EXIT_UTIL;
}

int Util::Ftruncate(int fd, off_t offset)
{
    ENTER_UTIL;
    // just use the fd, but if GZIP_IO or GNU_IO, flush first
    if (iotype == GZIP_IO || iotype == GNU_IO) {
        Fsync(fd);
    }
    ret = ftruncate(fd, offset);
    EXIT_UTIL;
}
