#ifndef __PATHLOCKS_H
#define __PATHLOCKS_H

#include "util.hh"


typedef enum LockType { ltRead, ltWrite, ltNone };

bool lockFile(int fd, LockType lockType, bool wait);


class PathLocks 
{
private:
    typedef pair<int, Path> FDPair;
    list<FDPair> fds;
    bool deletePaths;

public:
    PathLocks();
    PathLocks(const PathSet & paths,
        const string & waitMsg = "");
    void lockPaths(const PathSet & _paths,
        const string & waitMsg = "");
    ~PathLocks();
    void setDeletion(bool deletePaths);
};


#endif /* !__PATHLOCKS_H */
