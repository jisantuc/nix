# Instantiate.
storeExpr=$($TOP/src/nix-instantiate/nix-instantiate substitutes2.nix)
echo "store expr is $storeExpr"

# Find the output path.
outPath=$($TOP/src/nix-store/nix-store -qvvvvv "$storeExpr")
echo "output path is $outPath"

regSub() {
    (echo $1 && echo $2 && echo 3 && echo $outPath && echo Hallo && echo Wereld && echo 0) | $TOP/src/nix-store/nix-store --substitute
}

# Register a substitute for the output path.
regSub $outPath $(pwd)/substituter.sh

# Register another substitute for the output path.  This one takes
# precedence over the previous one.  It will fail.
regSub $outPath $(pwd)/substituter2.sh

$TOP/src/nix-store/nix-store -rvv "$storeExpr"

text=$(cat "$outPath"/hello)
if test "$text" != "Hallo Wereld"; then exit 1; fi
