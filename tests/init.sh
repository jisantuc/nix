echo "NIX_STORE_DIR=$NIX_STORE_DIR NIX_DB_DIR=$NIX_DB_DIR"

test -n "$TEST_ROOT"
if test -d "$TEST_ROOT"; then
    chmod -R u+w "$TEST_ROOT"
    rm -rf "$TEST_ROOT"
fi
mkdir "$TEST_ROOT"

mkdir "$NIX_STORE_DIR"
mkdir "$NIX_DATA_DIR"
mkdir "$NIX_LOG_DIR"
mkdir "$NIX_STATE_DIR"
mkdir "$NIX_DB_DIR"

mkdir $NIX_BIN_DIR
ln -s $TOP/src/nix-store/nix-store $NIX_BIN_DIR/
ln -s $TOP/src/nix-instantiate/nix-instantiate $NIX_BIN_DIR/
ln -s $TOP/src/nix-hash/nix-hash $NIX_BIN_DIR/
ln -s $TOP/scripts/nix-prefetch-url $NIX_BIN_DIR/
ln -s $TOP/scripts/nix-collect-garbage $NIX_BIN_DIR/
mkdir $NIX_BIN_DIR/nix
ln -s $TOP/scripts/download-using-manifests.pl $NIX_BIN_DIR/nix/
ln -s $TOP/scripts/readmanifest.pm $NIX_BIN_DIR/nix/

mkdir -p "$NIX_LOCALSTATE_DIR"/nix/manifests
mkdir -p "$NIX_LOCALSTATE_DIR"/nix/gcroots
mkdir -p "$NIX_LOCALSTATE_DIR"/log/nix

mkdir $NIX_DATA_DIR/nix
cp -prd $TOP/corepkgs $NIX_DATA_DIR/nix/
# Bah, script has the prefix hard-coded.
for i in \
    $NIX_DATA_DIR/nix/corepkgs/nar/nar.sh \
    $NIX_DATA_DIR/nix/corepkgs/fetchurl/builder.sh \
    $NIX_BIN_DIR/nix/download-using-manifests.pl \
    $NIX_BIN_DIR/nix-prefetch-url \
    $NIX_BIN_DIR/nix-collect-garbage \
    ; do
    echo "$REAL_BIN_DIR"
    sed < $i > $i.tmp \
        -e "s^$REAL_BIN_DIR^$NIX_BIN_DIR^" \
        -e "s^$REAL_LIBEXEC_DIR^$NIX_LIBEXEC_DIR^" \
        -e "s^$REAL_LOCALSTATE_DIR^$NIX_LOCALSTATE_DIR^" \
        -e "s^$REAL_DATA_DIR^$NIX_DATA_DIR^" \
        -e "s^$REAL_STORE_DIR^$NIX_STORE_DIR^"
    mv $i.tmp $i
    chmod +x $i
done

# Initialise the database.
$TOP/src/nix-store/nix-store --init

# Did anything happen?
test -e "$NIX_DB_DIR"/validpaths
