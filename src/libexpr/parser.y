%glr-parser
%pure-parser
%locations
%error-verbose
%defines
/* %no-lines */
%parse-param { yyscan_t scanner }
%parse-param { ParseData * data }
%lex-param { yyscan_t scanner }


%{
/* Newer versions of Bison copy the declarations below to
   parser-tab.hh, which sucks bigtime since lexer.l doesn't want that
   stuff.  So allow it to be excluded. */
#ifndef BISON_HEADER_HACK
#define BISON_HEADER_HACK
    
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aterm.hh"
#include "util.hh"
    
#include "parser-tab.hh"
#include "lexer-tab.hh"
#define YYSTYPE YYSTYPE // workaround a bug in Bison 2.4

#include "nixexpr.hh"
#include "nixexpr-ast.hh"


using namespace nix;


namespace nix {

    
struct ParseData 
{
    Expr result;
    Path basePath;
    Path path;
    string error;
};
 

static string showAttrPath(ATermList attrPath)
{
    string s;
    for (ATermIterator i(attrPath); i; ++i) {
        if (!s.empty()) s += '.';
        s += aterm2String(*i);
    }
    return s;
}
 

struct Tree
{
    Expr leaf; ATerm pos; bool recursive;
    typedef std::map<ATerm, Tree> Children;
    Children children;
    Tree() { leaf = 0; recursive = true; }
};


static ATermList buildAttrs(const Tree & t, ATermList & nonrec)
{
    ATermList res = ATempty;
    for (Tree::Children::const_reverse_iterator i = t.children.rbegin();
         i != t.children.rend(); ++i)
        if (!i->second.recursive)
            nonrec = ATinsert(nonrec, makeBind(i->first, i->second.leaf, i->second.pos));
        else
            res = ATinsert(res, i->second.leaf
                ? makeBind(i->first, i->second.leaf, i->second.pos)
                : makeBind(i->first, makeAttrs(buildAttrs(i->second, nonrec)), makeNoPos()));
    return res;
}
 

static Expr fixAttrs(bool recursive, ATermList as)
{
    Tree attrs;

    for (ATermIterator i(as); i; ++i) {
        ATermList names, attrPath; Expr src, e; ATerm name, pos;

        if (matchInherit(*i, src, names, pos)) {
            bool fromScope = matchScope(src);
            for (ATermIterator j(names); j; ++j) {
                Expr rhs = fromScope ? makeVar(*j) : makeSelect(src, *j);
                if (attrs.children.find(*j) != attrs.children.end()) 
                    throw ParseError(format("duplicate definition of attribute `%1%' at %2%")
                        % showAttrPath(ATmakeList1(*j)) % showPos(pos));
                Tree & t(attrs.children[*j]);
                t.leaf = rhs; t.pos = pos; if (recursive) t.recursive = false;
            }
        }

        else if (matchBindAttrPath(*i, attrPath, e, pos)) {

            Tree * t(&attrs);
            
            for (ATermIterator j(attrPath); j; ) {
                name = *j; ++j;
                if (t->leaf) throw ParseError(format("attribute set containing `%1%' at %2% already defined at %3%")
                    % showAttrPath(attrPath) % showPos(pos) % showPos (t->pos));
                t = &(t->children[name]);
            }

            if (t->leaf)
                throw ParseError(format("duplicate definition of attribute `%1%' at %2% and %3%")
                    % showAttrPath(attrPath) % showPos(pos) % showPos (t->pos));
            if (!t->children.empty())
                throw ParseError(format("duplicate definition of attribute `%1%' at %2%")
                    % showAttrPath(attrPath) % showPos(pos));

            t->leaf = e; t->pos = pos;
        }

        else abort(); /* can't happen */
    }

    ATermList nonrec = ATempty;
    ATermList rec = buildAttrs(attrs, nonrec);
        
    return recursive ? makeRec(rec, nonrec) : makeAttrs(rec);
}


static void checkPatternVars(ATerm pos, ATermMap & map, Pattern pat)
{
    ATerm name;
    ATermList formals;
    Pattern pat1, pat2;
    ATermBool ellipsis;
    if (matchVarPat(pat, name)) {
        if (map.get(name))
            throw ParseError(format("duplicate formal function argument `%1%' at %2%")
                % aterm2String(name) % showPos(pos));
        map.set(name, name);
    }
    else if (matchAttrsPat(pat, formals, ellipsis)) { 
        for (ATermIterator i(formals); i; ++i) {
            ATerm d1;
            if (!matchFormal(*i, name, d1)) abort();
            if (map.get(name))
                throw ParseError(format("duplicate formal function argument `%1%' at %2%")
                    % aterm2String(name) % showPos(pos));
            map.set(name, name);
        }
    }
    else if (matchAtPat(pat, pat1, pat2)) {
        checkPatternVars(pos, map, pat1);
        checkPatternVars(pos, map, pat2);
    }
    else abort();
}


static void checkPatternVars(ATerm pos, Pattern pat)
{
    ATermMap map;
    checkPatternVars(pos, map, pat);
}


static Expr stripIndentation(ATermList es)
{
    if (es == ATempty) return makeStr("");
    
    /* Figure out the minimum indentation.  Note that by design
       whitespace-only final lines are not taken into account.  (So
       the " " in "\n ''" is ignored, but the " " in "\n foo''" is.) */
    bool atStartOfLine = true; /* = seen only whitespace in the current line */
    unsigned int minIndent = 1000000;
    unsigned int curIndent = 0;
    ATerm e;
    for (ATermIterator i(es); i; ++i) {
        if (!matchIndStr(*i, e)) {
            /* Anti-quotations end the current start-of-line whitespace. */
            if (atStartOfLine) {
                atStartOfLine = false;
                if (curIndent < minIndent) minIndent = curIndent;
            }
            continue;
        }
        string s = aterm2String(e);
        for (unsigned int j = 0; j < s.size(); ++j) {
            if (atStartOfLine) {
                if (s[j] == ' ')
                    curIndent++;
                else if (s[j] == '\n') {
                    /* Empty line, doesn't influence minimum
                       indentation. */
                    curIndent = 0;
                } else {
                    atStartOfLine = false;
                    if (curIndent < minIndent) minIndent = curIndent;
                }
            } else if (s[j] == '\n') {
                atStartOfLine = true;
                curIndent = 0;
            }
        }
    }

    /* Strip spaces from each line. */
    ATermList es2 = ATempty;
    atStartOfLine = true;
    unsigned int curDropped = 0;
    unsigned int n = ATgetLength(es);
    for (ATermIterator i(es); i; ++i, --n) {
        if (!matchIndStr(*i, e)) {
            atStartOfLine = false;
            curDropped = 0;
            es2 = ATinsert(es2, *i);
            continue;
        }
        
        string s = aterm2String(e);
        string s2;
        for (unsigned int j = 0; j < s.size(); ++j) {
            if (atStartOfLine) {
                if (s[j] == ' ') {
                    if (curDropped++ >= minIndent)
                        s2 += s[j];
                }
                else if (s[j] == '\n') {
                    curDropped = 0;
                    s2 += s[j];
                } else {
                    atStartOfLine = false;
                    curDropped = 0;
                    s2 += s[j];
                }
            } else {
                s2 += s[j];
                if (s[j] == '\n') atStartOfLine = true;
            }
        }

        /* Remove the last line if it is empty and consists only of
           spaces. */
        if (n == 1) {
            string::size_type p = s2.find_last_of('\n');
            if (p != string::npos && s2.find_first_not_of(' ', p + 1) == string::npos)
                s2 = string(s2, 0, p + 1);
        }
            
        es2 = ATinsert(es2, makeStr(s2));
    }

    return makeConcatStrings(ATreverse(es2));
}


void backToString(yyscan_t scanner);
void backToIndString(yyscan_t scanner);


static Pos makeCurPos(YYLTYPE * loc, ParseData * data)
{
    return makePos(toATerm(data->path),
        loc->first_line, loc->first_column);
}

#define CUR_POS makeCurPos(yylocp, data)


}


void yyerror(YYLTYPE * loc, yyscan_t scanner, ParseData * data, const char * error)
{
    data->error = (format("%1%, at `%2%':%3%:%4%")
        % error % data->path % loc->first_line % loc->first_column).str();
}


/* Make sure that the parse stack is scanned by the ATerm garbage
   collector. */
static void * mallocAndProtect(size_t size)
{
    void * p = malloc(size);
    if (p) ATprotectMemory(p, size);
    return p;
}

static void freeAndUnprotect(void * p)
{
    ATunprotectMemory(p);
    free(p);
}

#define YYMALLOC mallocAndProtect
#define YYFREE freeAndUnprotect


#endif


%}

%union {
  ATerm t;
  ATermList ts;
  struct {
    ATermList formals;
    bool ellipsis;
  } formals;
}

%type <t> start expr expr_function expr_if expr_op
%type <t> expr_app expr_select expr_simple bind inheritsrc formal
%type <t> pattern pattern2
%type <ts> binds ids attrpath expr_list string_parts ind_string_parts
%type <formals> formals
%token <t> ID INT STR IND_STR PATH URI
%token IF THEN ELSE ASSERT WITH LET IN REC INHERIT EQ NEQ AND OR IMPL
%token DOLLAR_CURLY /* == ${ */
%token IND_STRING_OPEN IND_STRING_CLOSE
%token ELLIPSIS

%nonassoc IMPL
%left OR
%left AND
%nonassoc EQ NEQ
%right UPDATE
%left NEG
%left '+'
%right CONCAT
%nonassoc '?'
%nonassoc '~'

%%

start: expr { data->result = $1; };

expr: expr_function;

expr_function
  : pattern ':' expr_function
    { checkPatternVars(CUR_POS, $1); $$ = makeFunction($1, $3, CUR_POS); }
  | ASSERT expr ';' expr_function
    { $$ = makeAssert($2, $4, CUR_POS); }
  | WITH expr ';' expr_function
    { $$ = makeWith($2, $4, CUR_POS); }
  | LET binds IN expr_function
    { $$ = makeSelect(fixAttrs(true, ATinsert($2, makeBindAttrPath(ATmakeList1(toATerm("<let-body>")), $4, CUR_POS))), toATerm("<let-body>")); }
  | expr_if
  ;

expr_if
  : IF expr THEN expr ELSE expr
    { $$ = makeIf($2, $4, $6); }
  | expr_op
  ;

expr_op
  : '!' expr_op %prec NEG { $$ = makeOpNot($2); }
  | expr_op EQ expr_op { $$ = makeOpEq($1, $3); }
  | expr_op NEQ expr_op { $$ = makeOpNEq($1, $3); }
  | expr_op AND expr_op { $$ = makeOpAnd($1, $3); }
  | expr_op OR expr_op { $$ = makeOpOr($1, $3); }
  | expr_op IMPL expr_op { $$ = makeOpImpl($1, $3); }
  | expr_op UPDATE expr_op { $$ = makeOpUpdate($1, $3); }
  | expr_op '~' expr_op { $$ = makeSubPath($1, $3); }
  | expr_op '?' ID { $$ = makeOpHasAttr($1, $3); }
  | expr_op '+' expr_op { $$ = makeOpPlus($1, $3); }
  | expr_op CONCAT expr_op { $$ = makeOpConcat($1, $3); }
  | expr_app
  ;

expr_app
  : expr_app expr_select
    { $$ = makeCall($1, $2); }
  | expr_select { $$ = $1; }
  ;

expr_select
  : expr_select '.' ID
    { $$ = makeSelect($1, $3); }
  | expr_simple { $$ = $1; }
  ;

expr_simple
  : ID { $$ = makeVar($1); }
  | INT { $$ = makeInt(ATgetInt((ATermInt) $1)); }
  | '"' string_parts '"' {
      /* For efficiency, and to simplify parse trees a bit. */
      if ($2 == ATempty) $$ = makeStr(toATerm(""), ATempty);
      else if (ATgetNext($2) == ATempty) $$ = ATgetFirst($2);
      else $$ = makeConcatStrings(ATreverse($2));
  }
  | IND_STRING_OPEN ind_string_parts IND_STRING_CLOSE {
      $$ = stripIndentation(ATreverse($2));
  }
  | PATH { $$ = makePath(toATerm(absPath(aterm2String($1), data->basePath))); }
  | URI { $$ = makeStr($1, ATempty); }
  | '(' expr ')' { $$ = $2; }
  /* Let expressions `let {..., body = ...}' are just desugared
     into `(rec {..., body = ...}).body'. */
  | LET '{' binds '}'
    { $$ = makeSelect(fixAttrs(true, $3), toATerm("body")); }
  | REC '{' binds '}'
    { $$ = fixAttrs(true, $3); }
  | '{' binds '}'
    { $$ = fixAttrs(false, $2); }
  | '[' expr_list ']' { $$ = makeList(ATreverse($2)); }
  ;

string_parts
  : string_parts STR { $$ = ATinsert($1, $2); }
  | string_parts DOLLAR_CURLY expr '}' { backToString(scanner); $$ = ATinsert($1, $3); }
  | { $$ = ATempty; }
  ;

ind_string_parts
  : ind_string_parts IND_STR { $$ = ATinsert($1, $2); }
  | ind_string_parts DOLLAR_CURLY expr '}' { backToIndString(scanner); $$ = ATinsert($1, $3); }
  | { $$ = ATempty; }
  ;

pattern
  : pattern2 '@' pattern { $$ = makeAtPat($1, $3); }
  | pattern2
  ;

pattern2
  : ID { $$ = makeVarPat($1); }
  | '{' formals '}' { $$ = makeAttrsPat($2.formals, $2.ellipsis ? eTrue : eFalse); }
  ;

binds
  : binds bind { $$ = ATinsert($1, $2); }
  | { $$ = ATempty; }
  ;

bind
  : attrpath '=' expr ';'
    { $$ = makeBindAttrPath(ATreverse($1), $3, CUR_POS); }
  | INHERIT inheritsrc ids ';'
    { $$ = makeInherit($2, $3, CUR_POS); }
  ;

inheritsrc
  : '(' expr ')' { $$ = $2; }
  | { $$ = makeScope(); }
  ;

ids: ids ID { $$ = ATinsert($1, $2); } | { $$ = ATempty; };

attrpath
  : attrpath '.' ID { $$ = ATinsert($1, $3); }
  | ID { $$ = ATmakeList1($1); }
  ;

expr_list
  : expr_list expr_select { $$ = ATinsert($1, $2); }
  | { $$ = ATempty; }
  ;

formals
  : formal ',' formals /* !!! right recursive */
    { $$.formals = ATinsert($3.formals, $1); $$.ellipsis = $3.ellipsis; }
  | formal
    { $$.formals = ATinsert(ATempty, $1); $$.ellipsis = false; }
  |
    { $$.formals = ATempty; $$.ellipsis = false; }
  | ELLIPSIS
    { $$.formals = ATempty; $$.ellipsis = true; }
  ;

formal
  : ID { $$ = makeFormal($1, makeNoDefaultValue()); }
  | ID '?' expr { $$ = makeFormal($1, makeDefaultValue($3)); }
  ;
  
%%


#include "eval.hh"  

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>


namespace nix {
      

static Expr parse(EvalState & state,
    const char * text, const Path & path,
    const Path & basePath)
{
    yyscan_t scanner;
    ParseData data;
    data.basePath = basePath;
    data.path = path;

    yylex_init(&scanner);
    yy_scan_string(text, scanner);
    int res = yyparse(scanner, &data);
    yylex_destroy(scanner);
    
    if (res) throw ParseError(data.error);

    try {
        checkVarDefs(state.primOps, data.result);
    } catch (Error & e) {
        throw ParseError(format("%1%, in `%2%'") % e.msg() % path);
    }
    
    return data.result;
}


Expr parseExprFromFile(EvalState & state, Path path)
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
    while (true) {
        if (lstat(path.c_str(), &st))
            throw SysError(format("getting status of `%1%'") % path);
        if (!S_ISLNK(st.st_mode)) break;
        path = absPath(readLink(path), dirOf(path));
    }

    /* If `path' refers to a directory, append `/default.nix'. */
    if (stat(path.c_str(), &st))
        throw SysError(format("getting status of `%1%'") % path);
    if (S_ISDIR(st.st_mode))
        path = canonPath(path + "/default.nix");

    /* Read and parse the input file. */
    return parse(state, readFile(path).c_str(), path, dirOf(path));
}


Expr parseExprFromString(EvalState & state,
    const string & s, const Path & basePath)
{
    return parse(state, s.c_str(), "(string)", basePath);
}

 
}
