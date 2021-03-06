source common.sh

pullCache () {
    echo "pulling cache..."
    $NIX_BIN_DIR/nix-pull file://$TEST_ROOT/manifest
}

clearStore
clearManifests
pullCache

drvPath=$($nixinstantiate dependencies.nix)
outPath=$($nixstore -q $drvPath)

echo "building $outPath using substitutes..."
$nixstore -r $outPath

cat $outPath/input-2/bar

clearStore
clearManifests
pullCache

echo "building $drvPath using substitutes..."
$nixstore -r $drvPath

cat $outPath/input-2/bar

# Check that the derivers are set properly.
test $($nixstore -q --deriver "$outPath") = "$drvPath"
$nixstore -q --deriver $(readLink $outPath/input-2) | grep -q -- "-input-2.drv" 

clearManifests
