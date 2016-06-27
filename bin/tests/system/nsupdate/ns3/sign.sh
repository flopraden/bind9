#!/bin/sh -e
#
# Copyright (C) 2010-2012, 2014, 2016  Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

SYSTEMTESTTOP=../..
. $SYSTEMTESTTOP/conf.sh

zone=nsec3param.test.
infile=nsec3param.test.db.in
zonefile=nsec3param.test.db

keyname1=`$KEYGEN -q -r $RANDFILE -a NSEC3RSASHA1 -b 1024 -n zone -f KSK $zone`
keyname2=`$KEYGEN -q -r $RANDFILE -a NSEC3RSASHA1 -b 1024 -n zone $zone`

cat $infile $keyname1.key $keyname2.key >$zonefile

$SIGNER -P -3 - -H 1 -r $RANDFILE -o $zone -k $keyname1 $zonefile $keyname2 > /dev/null

zone=dnskey.test.
infile=dnskey.test.db.in
zonefile=dnskey.test.db

keyname1=`$KEYGEN -q -r $RANDFILE -a RSASHA1 -b 1024 -n zone -f KSK $zone`
keyname2=`$KEYGEN -q -r $RANDFILE -a RSASHA1 -b 1024 -n zone $zone`

cat $infile $keyname1.key $keyname2.key >$zonefile

$SIGNER -P -r $RANDFILE -o $zone -k $keyname1 $zonefile $keyname2 > /dev/null
