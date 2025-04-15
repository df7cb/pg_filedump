#!/bin/sh

sed -e "s/logid ....../logid ....../" \
    -e "s/recoff 0x......../recoff 0x......../" \
    -e "s/Checksum: 0x..../Checksum: 0x..../" \
    -e "s/id: ....../id: ....../g" \
    -e "s/ 8< .*/ 8< [snipped]/"
