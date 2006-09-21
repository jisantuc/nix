source common.sh

# This takes way to long on Cygwin (because process creation is so slow...).
if test "$system" = i686-cygwin; then exit 0; fi

max=2500

reference=$NIX_STORE_DIR/abcdef
touch $reference
(echo $reference && echo && echo 0) | $nixstore --register-validity 

echo "registering..."
time for ((n = 0; n < $max; n++)); do
    storePath=$NIX_STORE_DIR/$n
    touch $storePath
    (echo $storePath && echo && echo 1 && echo $reference)
done | $nixstore --register-validity 

echo "collecting garbage..."
time $nixstore --gc 2> /dev/null
