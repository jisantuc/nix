#! /bin/sh -e

#DIFF=zdc
DIFF=bsdiff

srcA=$1
srcB=$2

if test -z "$srcA" -o -z "$srcB"; then
    echo "syntax: bdiff.sh srcA srcB"
    exit 1
fi

(cd $srcB && find . -type f) | while read fn; do

    if test -f "$srcA/$fn"; then

        if ! test -f "$srcB/$fn"; then
            echo "DELETE $fn"
        else

            if ! cmp "$srcA/$fn" "$srcB/$fn" > /dev/null; then
                    
                echo "FILE DELTA FOR $fn"

                TMPFILE=/tmp/__bsdiff
                $DIFF "$srcA/$fn" "$srcB/$fn" $TMPFILE
                cat $TMPFILE

                diffSize=$(stat -c '%s' $TMPFILE)

                # For comparison.
                bzipSize=$(bzip2 < "$srcB/$fn" | wc -m)

                gain=$(echo "scale=2; ($diffSize - $bzipSize) / $bzipSize * 100" | bc)

                ouch=$(if test "${gain:0:1}" != "-"; then echo "!"; fi)

                printf "%7.2f %1s %10d %10d %s\n" \
                    "$gain" "$ouch" "$diffSize" "$bzipSize" "$fn" >&2
#        echo "$fn -> $diffSize $bzipSize ==> $gain  $ouch" >&2

                rm $TMPFILE

            fi

        fi

        # Note: be silent about unchanged files.

    else

	echo "NEW FILE $fn"

	cat "$srcB/$fn"

    fi

done
