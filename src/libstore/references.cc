#include <cerrno>
#include <map>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include "references.hh"
#include "hash.hh"


static unsigned int refLength = 32; /* characters */


static void search(const string & s,
    StringSet & ids, StringSet & seen)
{
    static bool initialised = false;
    static bool isBase32[256];
    if (!initialised) {
        for (unsigned int i = 0; i < 256; ++i) isBase32[i] = false;
        for (unsigned int i = 0; i < base32Chars.size(); ++i)
            isBase32[(unsigned char) base32Chars[i]] = true;
        initialised = true;
    }
    
    for (unsigned int i = 0; i + refLength <= s.size(); ) {
        int j;
        bool match = true;
        for (j = refLength - 1; j >= 0; --j)
            if (!isBase32[(unsigned char) s[i + j]]) {
                i += j + 1;
                match = false;
                break;
            }
        if (!match) continue;
        string ref(s, i, refLength);
        if (ids.find(ref) != ids.end()) {
            debug(format("found reference to `%1%' at offset `%2%'")
                  % ref % i);
            seen.insert(ref);
            ids.erase(ref);
        }
        ++i;
    }
}


void checkPath(const string & path,
    StringSet & ids, StringSet & seen)
{
    checkInterrupt();
    
    debug(format("checking `%1%'") % path);

    struct stat st;
    if (lstat(path.c_str(), &st))
        throw SysError(format("getting attributes of path `%1%'") % path);

    if (S_ISDIR(st.st_mode)) {
        Strings names = readDirectory(path);
	for (Strings::iterator i = names.begin(); i != names.end(); i++) {
            search(*i, ids, seen);
            checkPath(path + "/" + *i, ids, seen);
        }
    }

    else if (S_ISREG(st.st_mode)) {
        
        AutoCloseFD fd = open(path.c_str(), O_RDONLY);
        if (fd == -1) throw SysError(format("opening file `%1%'") % path);

        unsigned char * buf = new unsigned char[st.st_size];

        readFull(fd, buf, st.st_size);

        search(string((char *) buf, st.st_size), ids, seen);
        
        delete[] buf; /* !!! autodelete */
    }
    
    else if (S_ISLNK(st.st_mode))
        search(readLink(path), ids, seen);
    
    else throw Error(format("unknown file type: %1%") % path);
}


PathSet scanForReferences(const string & path, const PathSet & paths)
{
    map<string, Path> backMap;
    StringSet ids;
    StringSet seen;

    /* For efficiency (and a higher hit rate), just search for the
       hash part of the file name.  (This assumes that all references
       have the form `HASH-bla'). */
    for (PathSet::const_iterator i = paths.begin(); i != paths.end(); i++) {
        string baseName = baseNameOf(*i);
        string::size_type pos = baseName.find('-');
        if (pos == string::npos)
            throw Error(format("bad reference `%1%'") % *i);
        string s = string(baseName, 0, pos);
        assert(s.size() == refLength);
        assert(backMap.find(s) == backMap.end());
        // parseHash(htSHA256, s);
        ids.insert(s);
        backMap[s] = *i;
    }

    checkPath(path, ids, seen);

    PathSet found;
    for (StringSet::iterator i = seen.begin(); i != seen.end(); i++) {
        map<string, Path>::iterator j;
        if ((j = backMap.find(*i)) == backMap.end()) abort();
        found.insert(j->second);
    }

    return found;
}
