#! /usr/bin/perl -w -I/home/eelco/Dev/nix/scripts

use strict;
use readmanifest;

my $manifestDir = "/home/eelco/Dev/nix/patch/test";


# Check the arguments.
die unless scalar @ARGV == 1;
my $targetPath = $ARGV[0];


# Load all manifests.
my %narFiles;
my %patches;
my %successors;

for my $manifest (glob "$manifestDir/*.nixmanifest") {
    print STDERR "reading $manifest\n";
    readManifest $manifest, \%narFiles, \%patches, \%successors;
}


# Build a graph of all store paths that might contribute to the
# construction of $targetPath, and the special node "start".  The
# edges are either patch operations, or downloads of full NAR files.
# The latter edges only occur between "start" and a store path.

my %graph;

$graph{"start"} = {d => 0, pred => undef, edges => []};

my @queue = ();
my $queueFront = 0;
my %done;

sub addToQueue {
    my $v = shift;
    return if defined $done{$v};
    $done{$v} = 1;
    push @queue, $v;
}

sub addNode {
    my $u = shift;
    $graph{$u} = {d => 999999999999, pred => undef, edges => []}
        unless defined $graph{$u};
}

sub addEdge {
    my $u = shift;
    my $v = shift;
    my $w = shift;
    my $type = shift;
    my $info = shift;
    addNode $u;
    push @{$graph{$u}->{edges}},
        {weight => $w, start => $u, end => $v, type => $type, info => $info};
    my $n = scalar @{$graph{$u}->{edges}};
}

addToQueue $targetPath;

while ($queueFront < scalar @queue) {
    my $u = $queue[$queueFront++];
    print "$u\n";

    addNode $u;

    # If the path already exists, it has distance 0 from the "start"
    # node.
    system "nix-store --isvalid '$u' 2> /dev/null";
    if ($? == 0) {
        addEdge "start", $u, 0, "present", undef;
    }

    else {

        # Add patch edges.
        my $patchList = $patches{$u};
        foreach my $patch (@{$patchList}) {
            # !!! this should be cached
            my $hash = `nix-hash "$patch->{basePath}"`;
            chomp $hash;
            print "  MY HASH is $hash\n";
            if ($hash ne $patch->{baseHash}) {
                print "  REJECTING PATCH from $patch->{basePath}\n";
                next;
            }
            print "  PATCH from $patch->{basePath}\n";
            addToQueue $patch->{basePath};
            addEdge $patch->{basePath}, $u, $patch->{size}, "patch", $patch;
        }

        # Add NAR file edges to the start node.
        my $narFileList = $narFiles{$u};
        foreach my $narFile (@{$narFileList}) {
            print "  NAR from $narFile->{url}\n";
            addEdge "start", $u, $narFile->{size}, "narfile", $narFile;
        }

    }
}


# Run Dijkstra's shortest path algorithm to determine the shortest
# sequence of download and/or patch actions that will produce
# $targetPath.

sub byDistance { # sort by distance, reversed
    return -($graph{$a}->{d} <=> $graph{$b}->{d});
}

my @todo = keys %graph;

while (scalar @todo > 0) {

    # Remove the closest element from the todo list.
    @todo = sort byDistance @todo;
    my $u = pop @todo;

    my $u_ = $graph{$u};

    print "IN $u $u_->{d}\n";

    foreach my $edge (@{$u_->{edges}}) {
        my $v_ = $graph{$edge->{end}};
        if ($v_->{d} > $u_->{d} + $edge->{weight}) {
            $v_->{d} = $u_->{d} + $edge->{weight};
            # Store the edge; to edge->start is actually the
            # predecessor.
            $v_->{pred} = $edge; 
            print "  RELAX $edge->{end} $v_->{d}\n";
        }
    }
}


# Retrieve the shortest path from "start" to $targetPath.
my @path = ();
my $cur = $targetPath;
die "don't know how to produce $targetPath\n"
    unless defined $graph{$targetPath}->{pred};
while ($cur ne "start") {
    push @path, $graph{$cur}->{pred};
    $cur = $graph{$cur}->{pred}->{start};
}


# Traverse the shortest path, perform the actions described by the
# edges.
my $curStep = 1;
my $maxStep = scalar @path;

sub downloadFile {
    my $url = shift;
    my $hash = shift;
    $ENV{"PRINT_PATH"} = 1;
    $ENV{"QUIET"} = 1;
    my ($hash2, $path) = `nix-prefetch-url '$url' '$hash'`;
    chomp $hash2;
    chomp $path;
    die "hash mismatch" if $hash ne $hash2;
    return $path;
}

while (scalar @path > 0) {
    my $edge = pop @path;
    my $u = $edge->{start};
    my $v = $edge->{end};

    print "\n*** Step $curStep/$maxStep: ";
    $curStep++;

    if ($edge->{type} eq "present") {
        print "using already present path `$v'\n";
    }

    elsif ($edge->{type} eq "patch") {
        my $patch = $edge->{info};
        print "applying patch `$patch->{url}' to `$u' to create `$v'\n";

        # Download the patch.
        print "  downloading patch...\n";
        my $patchPath = downloadFile "$patch->{url}", "$patch->{hash}";

        # Turn the base path into a NAR archive, to which we can
        # actually apply the patch.
        print "  packing base path...\n";
        system "nix-store --dump $patch->{basePath} > /tmp/nar";
        die "cannot dump `$patch->{basePath}'" if ($? != 0);

        # Apply the patch.
        print "  applying patch...\n";
        system "bspatch /tmp/nar /tmp/nar2 $patchPath";
        die "cannot apply patch `$patchPath' to /tmp/nar" if ($? != 0);

        # Unpack the resulting NAR archive into the target path.
        print "  unpacking patched archive...\n";
        system "nix-store --restore $targetPath < /tmp/nar2";
        die "cannot unpack /tmp/nar2 into `$targetPath'" if ($? != 0);
    }

    elsif ($edge->{type} eq "narfile") {
        my $narFile = $edge->{info};
        print "downloading `$narFile->{url}' into `$v'\n";

        # Download the archive.
        print "  downloading archive...\n";
        my $narFilePath = downloadFile "$narFile->{url}", "$narFile->{hash}";

        # Unpack the archive into the target path.
        print "  unpacking archive...\n";
        system "bunzip2 < '$narFilePath' | nix-store --restore '$targetPath'";
        die "cannot unpack `$narFilePath' into `$targetPath'" if ($? != 0);
    }
}
