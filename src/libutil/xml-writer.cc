#include <assert.h>

#include "xml-writer.hh"


XMLWriter::XMLWriter(ostream & output)
    : output(output)
{
    output << "<?xml version='1.0' encoding='utf-8'?>\n";
    closed = false;
}


XMLWriter::~XMLWriter()
{
    close();
}


void XMLWriter::close()
{
    if (closed) return;
    while (!pendingElems.empty()) closeElement();
    closed = true;
}


void XMLWriter::openElement(const string & name,
    const XMLAttrs & attrs)
{
    assert(!closed);
    output << "<" << name;
    writeAttrs(attrs);
    output << ">";
    pendingElems.push_back(name);
}


void XMLWriter::closeElement()
{
    assert(!pendingElems.empty());
    output << "</" << pendingElems.back() << ">";
    pendingElems.pop_back();
    if (pendingElems.empty()) closed = true;
}


void XMLWriter::writeEmptyElement(const string & name,
    const XMLAttrs & attrs)
{
    assert(!closed);
    output << "<" << name;
    writeAttrs(attrs);
    output << " />";
}


void XMLWriter::writeCharData(const string & data)
{
    assert(!pendingElems.empty());
    for (unsigned int i = 0; i < data.size(); ++i) {
        char c = data[i];
        if (c == '<') output << "&lt;";
        else if (c == '&') output << "&amp;";
        else output << c;
    }
}


void XMLWriter::writeAttrs(const XMLAttrs & attrs)
{
    for (XMLAttrs::const_iterator i = attrs.begin(); i != attrs.end(); ++i) {
        output << " " << i->first << "=\"";
        for (unsigned int j = 0; j < i->second.size(); ++j) {
            char c = i->second[j];
            if (c == '"') output << "&quot;";
            else if (c == '<') output << "&lt;";
            else if (c == '&') output << "&amp;";
            else output << c;
        }
        output << "\"";
    }
}


#if 0
int main(int argc, char * * argv)
{
    XMLWriter doc(cout);
    
    //    OpenElement e(doc, "foo");

    doc.openElement("foo");

    doc.writeCharData("dit is een test &\n");
    doc.writeCharData("<foo>\n");

    for (int i = 0; i < 5; ++i) {
        XMLAttrs attrs;
        attrs["a"] = "b";
        attrs["bla"] = "<foo>'&\">";
        XMLOpenElement e(doc, "item", attrs);
        doc.writeCharData("x");
    }

    return 0;
}
#endif
