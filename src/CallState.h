#ifndef __CallState_H__
#define __CallState_H__

#include "COPYRIGHT.h"
#include <iostream>
using namespace std;

// example of CALL_INFO to pass to callstate 
// #define CALL_INFO FUNCTION,__FILE__,__LINE__,PLFS_INFO

class CallState 
{
    public:
        char *func;
        char *myfile;
        char *path;
        int l;
        int flags;

        CallState(const char *f, const char *m, int l, int flags, char *path) {
            this->func = f;
            this->myfile = m;
            this->line = l;
            this->flags = flags;
            this->path = path;
        }

        ~CallState() {
        }

        void exit(long long int retval){
            mlog(this->flags, "%s Exiting %s: %s -- %lld\n", this->myfile, this->func, this->path, retval);

        }

    private:
        void enter(){
            mlog(this->flags, "%s Entering %s: %s\n", this->myfile, this->func, this->path);
        }
};


#endif
