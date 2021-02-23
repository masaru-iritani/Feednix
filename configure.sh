#!/usr/bin/env bash
pushd `dirname $0` > /dev/null
autoreconf -i
./configure --sysconfdir=/etc
popd > /dev/null
