source common.sh

file=./add.sh

path=$($nixstore --add $file)

echo $path

hash=$($nixstore -q --hash $path)

echo $hash

test "$hash" = "sha256:$(nix-hash --type sha256 --base32 $file)"
