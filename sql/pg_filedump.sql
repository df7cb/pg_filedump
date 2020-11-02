-- 64 bit output in *.out, 32 bit output in *_3.out

\! pg_filedump testfile | sed -e 's/recoff 0x......../recoff 0x......../'
