-- PG14+ output in toast.out; PG13- output in toast_1.out

create table toast (
    description text,
    data text
);

insert into toast values ('short inline', 'xxx');
insert into toast values ('long inline uncompressed', repeat('x', 200));

alter table toast alter column data set storage external;
insert into toast values ('external uncompressed', repeat('0123456789 8< ', 200));

alter table toast alter column data set storage extended;
insert into toast values ('inline compressed pglz', repeat('0123456789 8< ', 200));
insert into toast values ('extended compressed pglz', repeat('0123456789 8< ', 20000));

alter table toast alter column data set compression lz4;
insert into toast values ('inline compressed lz4', repeat('0123456789 8< ', 200));
insert into toast values ('extended compressed lz4', repeat('0123456789 8< ', 50000));

vacuum toast;
checkpoint;

-- copy tables where client can read it
\set relname 'toast'
select oid as datoid from pg_database where datname = current_database() \gset
select relfilenode, reltoastrelid from pg_class where relname = :'relname' \gset
select lo_import(format('base/%s/%s', :'datoid', :'relfilenode')) as loid \gset
\set output :relname '.heap'
\lo_export :loid :output
select lo_import(format('base/%s/%s', :'datoid', :'reltoastrelid')) as toast_loid \gset
\set output :reltoastrelid
\lo_export :toast_loid :output

\setenv relname :relname
\! pg_filedump -D text,text $relname.heap | sed -e "s/logid      ./logid      ./" -e "s/recoff 0x......../recoff 0x......../"
\! pg_filedump -D text,text -t $relname.heap | sed -e "s/logid      ./logid      ./" -e "s/recoff 0x......../recoff 0x......../" -e 's/id:  ...../id:  ...../g' -e 's/ 8< .*//'
