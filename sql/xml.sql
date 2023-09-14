-- 64 bit output in *.out, 32 bit output in *_3.out
-- server without --with-libxml support output in *_1.out

select oid as datoid from pg_database where datname = current_database() \gset

----------------------------------------------------------------------------------------------

create table xml (x xml);
insert into xml values ('<xml></xml>'), (null);
\set relname xml
\ir run_test.sql
