clearStore () {
    echo "clearing store..."
    chmod -R +w "$NIX_STORE_DIR"
    rm -rf "$NIX_STORE_DIR"
    mkdir "$NIX_STORE_DIR"
    rm -rf "$NIX_DB_DIR"
    mkdir "$NIX_DB_DIR"
    $TOP/src/nix-store/nix-store --init
}

pullCache () {
    echo "pulling cache..."
    $PERL -w -I$TOP/scripts $TOP/scripts/nix-pull file://$TEST_ROOT/manifest
}

clearStore
pullCache

drvPath=$($TOP/src/nix-instantiate/nix-instantiate dependencies.nix)
outPath=$($TOP/src/nix-store/nix-store -q $drvPath)

echo "building $outPath using substitutes..."
$TOP/src/nix-store/nix-store -r $outPath

cat $outPath/input-2/bar

clearStore
pullCache

echo "building $drvPath using substitutes..."
$TOP/src/nix-store/nix-store -r $drvPath

cat $outPath/input-2/bar
