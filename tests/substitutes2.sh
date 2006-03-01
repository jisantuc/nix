source common.sh

# Instantiate.
drvPath=$($nixinstantiate substitutes2.nix)
echo "derivation is $drvPath"

# Find the output path.
outPath=$($nixstore -qvvvvv "$drvPath")
echo "output path is $outPath"

regSub() {
    (echo $1 && echo "" && echo $2 && echo 3 && echo $outPath && echo Hallo && echo Wereld && echo 0) | $nixstore --register-substitutes
}

# Register a substitute for the output path.
regSub $outPath $(pwd)/substituter.sh

# Register another substitute for the output path.  This one takes
# precedence over the previous one.  It will fail.
regSub $outPath $(pwd)/substituter2.sh

$nixstore -rvv "$drvPath"

text=$(cat "$outPath"/hello)
if test "$text" != "Hallo Wereld"; then exit 1; fi
