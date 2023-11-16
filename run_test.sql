\echo Testing :relname

vacuum :"relname";
checkpoint;

select relfilenode from pg_class where relname = :'relname' \gset
select lo_import(format('base/%s/%s', :'datoid', :'relfilenode')) as oid \gset
\set output :relname '.heap'
\lo_export :oid :output

\setenv relname :relname
\! $(${PG_CONFIG:-pg_config} --bindir)/pg_filedump -D $relname $relname.heap | sed -e "s/logid      ./logid      ./" -e "s/recoff 0x......../recoff 0x......../"

--
----------------------------------------------------------------------------------------------
--
