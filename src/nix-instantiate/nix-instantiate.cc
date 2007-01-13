#include <map>
#include <iostream>

#include "globals.hh"
#include "shared.hh"
#include "eval.hh"
#include "parser.hh"
#include "get-drvs.hh"
#include "attr-path.hh"
#include "expr-to-xml.hh"
#include "util.hh"
#include "store-api.hh"
#include "help.txt.hh"


using namespace nix;


void printHelp()
{
    std::cout << string((char *) helpText, sizeof helpText);
}


static Expr parseStdin(EvalState & state)
{
    startNest(nest, lvlTalkative, format("parsing standard input"));
    string s, s2;
    while (getline(std::cin, s2)) s += s2 + "\n";
    return parseExprFromString(state, s, absPath("."));
}


static Path gcRoot;
static int rootNr = 0;
static bool indirectRoot = false;


static void printResult(EvalState & state, Expr e,
    bool evalOnly, bool xmlOutput, const ATermMap & autoArgs)
{
    PathSet context;
    
    if (evalOnly)
        if (xmlOutput)
            printTermAsXML(e, std::cout, context);
        else
            std::cout << format("%1%\n") % canonicaliseExpr(e);
    
    else {
        DrvInfos drvs;
        getDerivations(state, e, "", autoArgs, drvs);
        for (DrvInfos::iterator i = drvs.begin(); i != drvs.end(); ++i) {
            Path drvPath = i->queryDrvPath(state);
            if (gcRoot == "")
                printGCWarning();
            else
                drvPath = addPermRoot(drvPath,
                    makeRootName(gcRoot, rootNr),
                    indirectRoot);
            std::cout << format("%1%\n") % drvPath;
        }
    }
}


void processExpr(EvalState & state, const Strings & attrPaths,
    bool parseOnly, bool strict, const ATermMap & autoArgs,
    bool evalOnly, bool xmlOutput, Expr e)
{
    for (Strings::const_iterator i = attrPaths.begin(); i != attrPaths.end(); ++i) {
        Expr e2 = findAlongAttrPath(state, *i, autoArgs, e);
        if (!parseOnly)
            if (strict)
                e2 = strictEvalExpr(state, e2);
            else
                e2 = evalExpr(state, e2);
        printResult(state, e2, evalOnly, xmlOutput, autoArgs);
    }
}


void run(Strings args)
{
    EvalState state;
    Strings files;
    bool readStdin = false;
    bool evalOnly = false;
    bool parseOnly = false;
    bool xmlOutput = false;
    bool strict = false;
    Strings attrPaths;
    ATermMap autoArgs(128);

    for (Strings::iterator i = args.begin();
         i != args.end(); )
    {
        string arg = *i++;

        if (arg == "-")
            readStdin = true;
        else if (arg == "--eval-only") {
            readOnlyMode = true;
            evalOnly = true;
        }
        else if (arg == "--parse-only") {
            readOnlyMode = true;
            parseOnly = evalOnly = true;
        }
        else if (arg == "--attr" || arg == "-A") {
            if (i == args.end())
                throw UsageError("`--attr' requires an argument");
            attrPaths.push_back(*i++);
        }
        else if (arg == "--arg") {
            if (i == args.end())
                throw UsageError("`--arg' requires two arguments");
            string name = *i++;
            if (i == args.end())
                throw UsageError("`--arg' requires two arguments");
            Expr value = parseExprFromString(state, *i++, absPath("."));
            autoArgs.set(toATerm(name), value);
        }
        else if (arg == "--add-root") {
            if (i == args.end())
                throw UsageError("`--add-root' requires an argument");
            gcRoot = absPath(*i++);
        }
        else if (arg == "--indirect")
            indirectRoot = true;
        else if (arg == "--xml")
            xmlOutput = true;
        else if (arg == "--strict")
            strict = true;
        else if (arg[0] == '-')
            throw UsageError(format("unknown flag `%1%'") % arg);
        else
            files.push_back(arg);
    }

    if (attrPaths.empty()) attrPaths.push_back("");

    store = openStore();

    if (readStdin) {
        Expr e = parseStdin(state);
        processExpr(state, attrPaths, parseOnly, strict, autoArgs,
            evalOnly, xmlOutput, e);
    }

    for (Strings::iterator i = files.begin();
         i != files.end(); i++)
    {
        Path path = absPath(*i);
        Expr e = parseExprFromFile(state, path);
        processExpr(state, attrPaths, parseOnly, strict, autoArgs,
            evalOnly, xmlOutput, e);
    }

    printEvalStats(state);
}


string programId = "nix-instantiate";
