#include "globals.hh"

#include <map>
#include <algorithm>


string nixStore = "/UNINIT";
string nixDataDir = "/UNINIT";
string nixLogDir = "/UNINIT";
string nixStateDir = "/UNINIT";
string nixDBPath = "/UNINIT";
string nixConfDir = "/UNINIT";

bool keepFailed = false;
bool keepGoing = false;
bool tryFallback = false;
Verbosity buildVerbosity = lvlInfo;
unsigned int maxBuildJobs = 1;
bool readOnlyMode = false;


static bool settingsRead = false;

static map<string, Strings> settings;


string & at(Strings & ss, unsigned int n)
{
    Strings::iterator i =ss.begin();
    advance(i, n);
    return *i;
}


static void readSettings()
{
    Path settingsFile = (format("%1%/%2%") % nixConfDir % "nix.conf").str();
    if (!pathExists(settingsFile)) return;
    string contents = readFile(settingsFile);

    unsigned int pos = 0;

    while (pos < contents.size()) {
        string line;
        while (pos < contents.size() && contents[pos] != '\n')
            line += contents[pos++];
        pos++;

        unsigned int hash = line.find('#');
        if (hash != string::npos)
            line = string(line, 0, hash);

        Strings tokens = tokenizeString(line);
        if (tokens.empty()) continue;

        if (tokens.size() < 2 || at(tokens, 1) != "=")
            throw Error(format("illegal configuration line `%1%' in `%2%'") % line % settingsFile);

        string name = at(tokens, 0);

        Strings::iterator i = tokens.begin();
        advance(i, 2);
        settings[name] = Strings(i, tokens.end());
    };
    
    settingsRead = true;
}


Strings querySetting(const string & name, const Strings & def)
{
    if (!settingsRead) readSettings();
    map<string, Strings>::iterator i = settings.find(name);
    return i == settings.end() ? def : i->second;
}


bool queryBoolSetting(const string & name, bool def)
{
    Strings defs;
    if (def) defs.push_back("true"); else defs.push_back("false");
    
    Strings value = querySetting(name, defs);
    if (value.size() != 1)
        throw Error(format("configuration option `%1%' should be either `true' or `false', not a list")
            % name);
    
    string v = value.front();
    if (v == "true") return true;
    else if (v == "false") return false;
    else throw Error(format("configuration option `%1%' should be either `true' or `false', not `%2%'")
        % name % v);
}
