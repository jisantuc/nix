source common.sh

clearStore
clearProfiles

checkRef() {
    nix-store -q --references ./result | grep -q "$1" || fail "missing reference $1"
}

# Test the export of the runtime dependency graph.

outPath=$($nixbuild ./export-graph.nix -A runtimeGraph)

test $(nix-store -q --references ./result | wc -l) = 2 || fail "bad nr of references"

checkRef input-2
for i in $(cat $outPath); do checkRef $i; done

# Test the export of the build-time dependency graph.

outPath=$($nixbuild ./export-graph.nix -A buildGraph)

checkRef input-1
checkRef input-2

for i in $(cat $outPath); do checkRef $i; done
