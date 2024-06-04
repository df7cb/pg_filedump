#!/usr/bin/perl

use strict;
use warnings;
use Config;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Spec;
use IPC::Run qw( run timeout );


note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('master');
$node->init(extra => ["--data-checksums"]);
$node->append_conf('postgresql.conf', 'fsync = True');
$node->start;

my $query = qq(
    create table t1(a int, b text, c bigint, filler char(400));
    insert into t1 values (1, 'asdasd1', 29347293874234444);
    insert into t1 values (2, 'asdasd2', 29347293874234445);
    insert into t1 values (3, 'asdasd', 29347293874234446);
    insert into t1 values (4, 'asdasd', 29347293874234447);
    checkpoint;
);
$node->safe_psql('postgres', $query);

note "running tests";

test_basic_output();
test_btree_output();
test_spgist_output();
test_gin_output();

$node->stop;
done_testing();

sub get_table_location
{
    return  File::Spec->catfile(
        $node->data_dir,
        $node->safe_psql('postgres', qq(SELECT pg_relation_filepath('@_');))
    );
}

sub run_pg_filedump
{
    my ($rel, @options) = @_;
    my ($stdout, $stderr);

    my $loc = get_table_location($rel);
    my $cmd = [ 'pg_filedump', @options, $loc ];
    my $result = run $cmd, '>', \$stdout, '2>', \$stderr
        or die "Error: could not execute pg_filedump";

    ok($stdout !~ qr/Error/, "error not found");

    return $stdout;
}

sub test_basic_output
{
    my $out_ = run_pg_filedump('t1', ("-D", "int,text,bigint"));

    ok($out_ =~ qr/Header/, "Header found");
    ok($out_ =~ qr/COPY: 1/, "first COPY found");
    ok($out_ =~ qr/COPY: 2/, "second COPY found");
    ok($out_ =~ qr/COPY: 3/, "third COPY found");
    ok($out_ =~ qr/COPY: 4/, "fourth COPY found");
    ok($out_ =~ qr/29347293874234447/, "number found");
    ok($out_ =~ qr/asdasd/, "string found");
}

sub test_btree_output
{
    my $query = qq(
        insert into t1 select * FROM generate_series(1, 10000);
        create index i1 on t1(b);
        checkpoint;
    );
    $node->safe_psql('postgres', $query);

    my $out_ = run_pg_filedump('i1', ('-i'));

    ok($out_ =~ qr/Header/, "Header found");
    ok($out_ =~ qr/BTree Index Section/, "BTree Index Section found");
    ok($out_ =~ qr/BTree Meta Data/, "BTree Meta Data found");
    ok($out_ =~ qr/Item   3/, "Item found");
    ok($out_ =~ qr/Previous/, "Previous item found");
    ok($out_ =~ qr/Next/, "Next item found");
    ok($out_ =~ qr/Level/, "Level found");
    ok($out_ !~ qr/Next XID/, "Next XID not found");

    # make leaf with BTP_DELETED flag
    $node->safe_psql('postgres', "delete from t1 where a >= 2000 and a < 4000;");
    $node->safe_psql('postgres', "vacuum t1; checkpoint;");

    $out_ = run_pg_filedump('i1', ('-i'));

    ok($out_ =~ qr/Next XID/, "Next XID found");
}

sub test_spgist_output
{
    $node->safe_psql('postgres', "create index i2 on t1 using spgist(b); checkpoint;");

    my $out_ = run_pg_filedump('i2');

    ok($out_ =~ qr/Header/, "Header found");
    ok($out_ =~ qr/SPGIST Index Section/, "SPGIST Index Section found");
    ok($out_ =~ qr/Item   4/, "Item found");
}

sub test_gin_output
{
    my $query = qq(
        create extension btree_gin;
        create index i3 on t1 using gin(b);
        checkpoint;
    );
    $node->safe_psql('postgres', $query);

    my $out_ = run_pg_filedump('i3');

    ok($out_ =~ qr/Header/, "Header found");
    ok($out_ =~ qr/GIN Index Section/, "GIN Index Section found");
    ok($out_ =~ qr/ItemPointer   3/, "Item found");
}
