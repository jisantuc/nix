#include <vector>
#include <iostream>
#include <cstdio>
#include <string>

using namespace std;


struct Decoder
{
    enum { stTop, stEscape, stCSI } state;
    string line;
    bool inHeader;
    int level;
    vector<int> args;
    bool newNumber;
    int priority;

    Decoder()
    {
        state = stTop;
        line = "";
        inHeader = false;
        level = 0;
        priority = 1;
    }

    void pushChar(char c);

    void finishLine();
};


void Decoder::pushChar(char c)
{
    switch (state) {
        
        case stTop:
            if (c == '\e') {
                state = stEscape;
            } else if (c == '\n') {
                finishLine();
            } else line += c;
            break;

        case stEscape:
            if (c == '[') {
                state = stCSI;
                args.clear();
                newNumber = true;
            } else
                state = stTop; /* !!! wrong */
            break;

        case stCSI:
            if (c >= 0x40 && c != 0x7e) {
                state = stTop;
                switch (c) {
                    case 'p':
                        if (line.size()) finishLine();
                        level++;
                        inHeader = true;
                        cout << "<nest>" << endl;
                        priority = args.size() >= 1 ? args[0] : 1;
                        break;
                    case 'q':
                        if (line.size()) finishLine();
                        if (level > 0) {
                            level--;
                            cout << "</nest>" << endl;
                        } else
                            cerr << "not enough nesting levels" << endl;
                        break;
                    case 's':
                        if (line.size()) finishLine();
                        priority = args.size() >= 1 ? args[0] : 1;
                        break;
                }
            } else if (c >= '0' && c <= '9') {
                int n = 0;
                if (!newNumber) {
                    n = args.back() * 10;
                    args.pop_back();
                }
                n += c - '0';
                args.push_back(n);
            }
            break;
            
    }
}


void Decoder::finishLine()
{
    string storeDir = "/nix/store/";
    int sz = storeDir.size();
    string tag = inHeader ? "head" : "line";
    cout << "<" << tag;
    if (priority != 1) cout << " priority='" << priority << "'";
    cout << ">";

    for (int i = 0; i < line.size(); i++) {

        if (line[i] == '<') cout << "&lt;";
        else if (line[i] == '&') cout << "&amp;";
        else if (i + sz + 33 < line.size() &&
            string(line, i, sz) == storeDir &&
            line[i + sz + 32] == '-')
        {
            int j = i + sz + 32;
            /* skip name */
            while (!strchr("/\n\r\t ()[]:;?<>", line[j])) j++;
            int k = j;
            while (!strchr("\n\r\t ()[]:;?<>", line[k])) k++;
            // !!! escaping
            cout << "<storeref>"
                 << "<storedir>"
                 << string(line, i, sz)
                 << "</storedir>"
                 << "<hash>"
                 << string(line, i + sz, 32)
                 << "</hash>"
                 << "<name>"
                 << string(line, i + sz + 32, j - (i + sz + 32))
                 << "</name>"
                 << "<path>"
                 << string(line, j, k - j)
                 << "</path>"
                 << "</storeref>";
            i = k - 1;
        } else cout << line[i];
    }
    
    cout << "</" << tag << ">" << endl;
    line = "";
    inHeader = false;
    priority = 1;
}


int main(int argc, char * * argv)
{
    Decoder dec;
    int c;

    cout << "<logfile>" << endl;
    
    while ((c = getchar()) != EOF) {
        dec.pushChar(c);
    }

    cout << "</logfile>" << endl;
}
