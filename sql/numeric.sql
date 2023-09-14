-- 64 bit output in *.out, 32 bit output in *_3.out
-- PG14+ output in *.out/*_3.out, earlier in *_1.out/*_4.out

select oid as datoid from pg_database where datname = current_database() \gset

----------------------------------------------------------------------------------------------

create table numeric (x numeric);
insert into numeric values (0), ('12341234'), ('-567890'), ('NaN'), (null);
insert into numeric values ('-Infinity'), ('Infinity'); -- needs PG 14
\set relname numeric
\ir run_test.sql
