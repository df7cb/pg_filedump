-- 64 bit output in *.out, 32 bit output in *_3.out
-- PG12+ output in *.out/*_3.out, earlier in *_1.out/*_4.out

select oid as datoid from pg_database where datname = current_database() \gset

----------------------------------------------------------------------------------------------

create table float4 (x float4);
insert into float4 values (0), ('-0'), ('-infinity'), ('infinity'), ('NaN'), (null);
\set relname float4
\ir run_test.sql

create table float8 (x float8);
insert into float8 values (0), ('-0'), ('-infinity'), ('infinity'), ('NaN'), (null);
\set relname float8
\ir run_test.sql
