#include <sstream>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <sglr.h>
#include <asfix2.h>
}

#include "aterm.hh"
#include "parser.hh"
#include "parse-table.h"


/* Cleanup cleans up an imploded parse tree into an actual abstract
   syntax tree that we can evaluate.  It removes quotes around
   strings, converts integer literals into actual integers, and
   absolutises paths relative to the directory containing the input
   file. */
struct Cleanup : TermFun
{
    string basePath;

    virtual ATerm operator () (ATerm e)
    {
        checkInterrupt();
        
        ATMatcher m;
        string s;

        if (atMatch(m, e) >> "Str" >> s)
            return ATmake("Str(<str>)",
                string(s, 1, s.size() - 2).c_str());

        if (atMatch(m, e) >> "Path" >> s)
            return ATmake("Path(<str>)", absPath(s, basePath).c_str());

        if (atMatch(m, e) >> "Int" >> s) {
            istringstream s2(s);
            int n;
            s2 >> n;
            return ATmake("Int(<int>)", n);
        }

        if (atMatch(m, e) >> "Var" >> "true")
            return ATmake("Bool(True)");
        
        if (atMatch(m, e) >> "Var" >> "false")
            return ATmake("Bool(False)");

        if (atMatch(m, e) >> "ExprNil")
            return (ATerm) ATempty;

        ATerm e1;
        ATermList e2;
        if (atMatch(m, e) >> "ExprCons" >> e1 >> e2)
            return (ATerm) ATinsert(e2, e1);

        return e;
    }
};


static Expr parse(const char * text, const string & location,
    const Path & basePath)
{
    /* Initialise the SDF libraries. */
    static bool initialised = false;
    static ATerm parseTable = 0;
    static language lang = 0;

    if (!initialised) {
        PT_initMEPTApi();
        PT_initAsFix2Api();
        SGinitParser(ATfalse);

        ATprotect(&parseTable);
        parseTable = ATreadFromBinaryString(
            (char *) nixParseTable, sizeof nixParseTable);
        if (!parseTable)
            throw Error(format("cannot construct parse table term"));

        ATprotect(&lang);
        lang = ATmake("Nix");
        if (!SGopenLanguageFromTerm("nix-parse", lang, parseTable))
            throw Error(format("cannot open language"));

        SG_STARTSYMBOL_ON();
        SG_OUTPUT_ON();
        SG_ASFIX2ME_ON();
        SG_AMBIGUITY_ERROR_ON();
        SG_FILTER_OFF();

        initialised = true;
    }

    /* Parse it. */
    ATerm result = SGparseString(lang, "Expr", (char *) text);
    if (!result)
        throw SysError(format("parse failed in `%1%'") % location);
    if (SGisParseError(result))
        throw Error(format("parse error in `%1%': %2%")
            % location % result);

    /* Implode it. */
    PT_ParseTree tree = PT_makeParseTreeFromTerm(result);
    if (!tree)
        throw Error(format("cannot create parse tree"));
    
    ATerm imploded = PT_implodeParseTree(tree,
        ATtrue,
        ATtrue,
        ATtrue,
        ATtrue,
        ATtrue,
        ATtrue,
        ATfalse,
        ATtrue,
        ATtrue,
        ATtrue,
        ATfalse);
    if (!imploded)
        throw Error(format("cannot implode parse tree"));

    printMsg(lvlVomit, format("imploded parse tree of `%1%': %2%")
        % location % imploded);

    /* Finally, clean it up. */
    Cleanup cleanup;
    cleanup.basePath = basePath;
    return bottomupRewrite(cleanup, imploded);
}


Expr parseExprFromFile(Path path)
{
    assert(path[0] == '/');

#if 0
    /* Perhaps this is already an imploded parse tree? */
    Expr e = ATreadFromNamedFile(path.c_str());
    if (e) return e;
#endif

    /* If `path' is a symlink, follow it.  This is so that relative
       path references work. */
    struct stat st;
    if (lstat(path.c_str(), &st))
        throw SysError(format("getting status of `%1%'") % path);
    if (S_ISLNK(st.st_mode)) path = absPath(readLink(path), dirOf(path));

    /* If `path' refers to a directory, append `/default.nix'. */
    if (stat(path.c_str(), &st))
        throw SysError(format("getting status of `%1%'") % path);
    if (S_ISDIR(st.st_mode))
        path = canonPath(path + "/default.nix");

    /* Read the input file.  We can't use SGparseFile() because it's
       broken, so we read the input ourselves and call
       SGparseString(). */
    AutoCloseFD fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) throw SysError(format("opening `%1%'") % path);

    if (fstat(fd, &st) == -1)
        throw SysError(format("statting `%1%'") % path);

    char text[st.st_size + 1];
    readFull(fd, (unsigned char *) text, st.st_size);
    text[st.st_size] = 0;

    return parse(text, path, dirOf(path));
}


Expr parseExprFromString(const string & s, const Path & basePath)
{
    return parse(s.c_str(), "(string)", basePath);
}
