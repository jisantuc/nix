#ifndef __PATHLOCKS_H
#define __PATHLOCKS_H

#include "util.hh"


typedef enum LockType { ltRead, ltWrite, ltNone };

bool lockFile(int fd, LockType lockType, bool wait);


class PathLocks 
{
private:
    list<int> fds;
    Paths paths;
    bool deletePaths;

public:
    PathLocks();
    PathLocks(const PathSet & paths);
    void lockPaths(const PathSet & _paths);
    ~PathLocks();
    void setDeletion(bool deletePaths);
};


#endif /* !__PATHLOCKS_H */
