-- 64 bit output in *.out, 32 bit output in *_3.out

select oid as datoid from pg_database where datname = current_database() \gset

----------------------------------------------------------------------------------------------

create table "int,text" (i int, t text);
insert into "int,text" values (1, 'one'), (null, 'two'), (3, null), (4, 'four');
\set relname int,text
\ir run_test.sql

-- do one test without options
\! pg_filedump int,text.heap | sed -e 's/logid      ./logid      ./' -e 's/recoff 0x......../recoff 0x......../'

----------------------------------------------------------------------------------------------

create table bigint (x bigint);
insert into bigint values (-1), (0), (1), (null);
\set relname bigint
\ir run_test.sql

create table bool (x bool);
insert into bool values (true), (false), (null);
\set relname bool
\ir run_test.sql

create table char (x "char");
insert into char values ('x'), (null);
\set relname char
\ir run_test.sql

create table "charN" (x char(5));
insert into "charN" values ('x'), ('xxxxx'), (null);
\set relname charN
\ir run_test.sql

create table date (x date);
insert into date values ('2000-01-01'), ('1900-02-02'), ('2100-12-31'), ('100-01-01 BC'), ('-infinity'), ('infinity'), (null);
\set relname date
\ir run_test.sql

create table int (x int);
insert into int values (-1), (0), (1), (null);
\set relname int
\ir run_test.sql

create table json (x json);
insert into json values ('1'), ('"one"'), ('{"a":"b"}'), ('null'), (null);
\set relname json
\ir run_test.sql

create table macaddr (x macaddr);
insert into macaddr values ('00:10:20:30:40:50'), (null);
\set relname macaddr
\ir run_test.sql

create table name (x name);
insert into name values ('name'), ('1234567890123456789012345678901234567890123456789012345678901234567890'), (null);
\set relname name
\ir run_test.sql

create table oid (x oid);
insert into oid values (-1), (0), (1), (null);
\set relname oid
\ir run_test.sql

create table smallint (x smallint);
insert into smallint values (-1), (0), (1), (null);
\set relname smallint
\ir run_test.sql

create table text (x text);
insert into text values ('hello world'), (null);
\set relname text
\ir run_test.sql

create table time (x time);
insert into time values ('00:00'), ('23:59:59'), ('23:59:60'), (null);
\set relname time
\ir run_test.sql

create table timestamp (x timestamp);
insert into timestamp values ('2000-01-01 00:00'), ('100-01-01 BC 2:22'), ('infinity'), ('-infinity'), (null);
\set relname timestamp
\ir run_test.sql

set timezone = 'Etc/UTC';
create table timestamptz (x timestamptz);
insert into timestamptz values ('2000-01-01 00:00'), ('100-01-01 BC 2:22'), ('infinity'), ('-infinity'), (null);
\set relname timestamptz
\ir run_test.sql

create table timetz (x timetz);
insert into timetz values ('00:00 Etc/UTC'), ('23:59:59 Etc/UTC'), ('23:59:60 Etc/UTC'), ('1:23+4:56'), (null);
\set relname timetz
\ir run_test.sql

create table uuid (x uuid);
insert into uuid values ('b4f0e2d6-429b-48bd-af06-6578829dd980'), ('00000000-0000-0000-0000-000000000000'), (null);
\set relname uuid
\ir run_test.sql

create table varchar (x varchar);
insert into varchar values ('Hello World'), (''), (null);
\set relname varchar
\ir run_test.sql

create table "varcharN" (x varchar(11));
insert into "varcharN" values ('Hello World'), (''), (null);
\set relname varcharN
\ir run_test.sql

create table xid (x xid);
insert into xid values ('-1'), ('0'), ('1'), (null);
\set relname xid
\ir run_test.sql
