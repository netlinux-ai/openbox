#!/bin/bash

[[ -e Makefile ]] || ./configure

make

./openbox/openbox --config-file $PWD/data/rc.xml
