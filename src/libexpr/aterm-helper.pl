#! /usr/bin/perl -w

die if scalar @ARGV != 2;

my $syms = "";
my $init = "";

open HEADER, ">$ARGV[0]";
open IMPL, ">$ARGV[1]";

while (<STDIN>) {
    next if (/^\s*$/);
    
    if (/^\s*(\w+)\s*\|([^\|]*)\|\s*(\w+)\s*\|\s*(\w+)?/) {
        my $const = $1;
        my @types = split ' ', $2;
        my $result = $3;
        my $funname = $4;
        $funname = $const unless defined $funname;

        my $formals = "";
        my $formals2 = "";
        my $args = "";
        my $unpack = "";
        my $n = 1;
        foreach my $type (@types) {
            $args .= ", ";
            if ($type eq "string") {
#                $args .= "(ATerm) ATmakeAppl0(ATmakeAFun((char *) e$n, 0, ATtrue))";
#                $type = "const char *";
                $type = "ATerm";
                $args .= "e$n";
            } elsif ($type eq "int") {
                $args .= "(ATerm) ATmakeInt(e$n)";
            } elsif ($type eq "ATermList" || $type eq "ATermBlob") {
                $args .= "(ATerm) e$n";
            } else {
                $args .= "e$n";
            }
            $formals .= ", " if $formals ne "";
            $formals .= "$type e$n";
            $formals2 .= ", ";
            $formals2 .= "$type & e$n";
            my $m = $n - 1;
            if ($type eq "int") {
                $unpack .= "    e$n = ATgetInt((ATermInt) ATgetArgument(e, $m));\n";
            } elsif ($type eq "ATermList") {
                $unpack .= "    e$n = (ATermList) ATgetArgument(e, $m);\n";
            } elsif ($type eq "ATermBlob") {
                $unpack .= "    e$n = (ATermBlob) ATgetArgument(e, $m);\n";
            } else {
                $unpack .= "    e$n = ATgetArgument(e, $m);\n";
            }
            $n++;
        }

        my $arity = scalar @types;

        print HEADER "extern AFun sym$funname;\n\n";
        
        print IMPL "AFun sym$funname = 0;\n";
        
        print HEADER "static inline $result make$funname($formals) {\n";
        print HEADER "    return (ATerm) ATmakeAppl$arity(sym$funname$args);\n";
        print HEADER "}\n\n";

        print HEADER "#ifdef __cplusplus\n";
        print HEADER "static inline bool match$funname(ATerm e$formals2) {\n";
        print HEADER "    if (ATgetAFun(e) != sym$funname) return false;\n";
        print HEADER "$unpack";
        print HEADER "    return true;\n";
        print HEADER "}\n";
        print HEADER "#endif\n\n\n";

        $init .= "    sym$funname = ATmakeAFun(\"$const\", $arity, ATfalse);\n";
        $init .= "    ATprotectAFun(sym$funname);\n";
    }

    elsif (/^\s*(\w+)\s*=\s*(.*)$/) {
        my $name = $1;
        my $value = $2;
        print HEADER "extern ATerm $name;\n";
        print IMPL "ATerm $name = 0;\n";
        $init .= "    $name = $value;\n";
    }

    else {
        die "bad line: `$_'";
    }
}

print HEADER "void initSyms();\n\n";

print HEADER "static inline ATerm string2ATerm(const char * s) {\n";
print HEADER "    return (ATerm) ATmakeAppl0(ATmakeAFun((char *) s, 0, ATtrue));\n";
print HEADER "}\n\n";

print HEADER "static inline const char * aterm2String(ATerm t) {\n";
print HEADER "    return (const char *) ATgetName(ATgetAFun(t));\n";
print HEADER "}\n\n";

print IMPL "\n";
print IMPL "void initSyms() {\n";
print IMPL "$init";
print IMPL "}\n";

close HEADER;
close IMPL;
