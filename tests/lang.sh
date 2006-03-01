source common.sh

fail=0

for i in lang/parse-fail-*.nix; do
    echo "parsing $i (should fail)";
    i=$(basename $i .nix)
    if $TOP/src/nix-instantiate/nix-instantiate --parse-only - < lang/$i.nix; then
        echo "FAIL: $i shouldn't parse"
        fail=1
    fi
done

for i in lang/parse-okay-*.nix; do
    echo "parsing $i (should succeed)";
    i=$(basename $i .nix)
    if ! $TOP/src/nix-instantiate/nix-instantiate --parse-only - < lang/$i.nix > lang/$i.ast; then
        echo "FAIL: $i should parse"
        fail=1
    fi
    if ! $aterm_bin/atdiff lang/$i.ast lang/$i.exp; then
        echo "FAIL: parse tree of $i not as expected"
        fail=1
    fi
done

for i in lang/eval-fail-*.nix; do
    echo "evaluating $i (should fail)";
    i=$(basename $i .nix)
    if $TOP/src/nix-instantiate/nix-instantiate --eval-only - < lang/$i.nix; then
        echo "FAIL: $i shouldn't evaluate"
        fail=1
    fi
done

for i in lang/eval-okay-*.nix; do
    echo "evaluating $i (should succeed)";
    i=$(basename $i .nix)
    if ! $TOP/src/nix-instantiate/nix-instantiate --eval-only - < lang/$i.nix > lang/$i.out; then
        echo "FAIL: $i should evaluate"
        fail=1
    fi
    if ! $aterm_bin/atdiff lang/$i.out lang/$i.exp; then
        echo "FAIL: evaluation result of $i not as expected"
        fail=1
    fi
done

exit $fail