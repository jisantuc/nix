source common.sh

export NIX_BUILD_HOOK="build-hook.hook.sh"

drvPath=$($nixinstantiate build-hook.nix)

echo "derivation is $drvPath"

outPath=$($nixstore -quf "$drvPath")

echo "output path is $outPath"

text=$(cat "$outPath"/foobar)
if test "$text" != "BARBAR"; then exit 1; fi
