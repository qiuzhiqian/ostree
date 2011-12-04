#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -e

echo "1..4"

. libtest.sh

setup_test_repository "regular"
cd ${test_tmpdir}
mkdir foo
cd foo
echo hi > hi
ln -s hi hello
mkdir subdir
echo contents > subdir/more
mkdir subdir/1
touch subdir/1/empty
mkdir subdir/2
touch subdir/2/empty
echo not > subdir/2/notempty

tar -c -z -f ../foo.tar.gz .
cd ..
$OSTREE commit -s "from tar" -b test-tar --tar foo.tar.gz
echo "ok tar commit"

cd ${test_tmpdir}
$OSTREE checkout test-tar test-tar-checkout
cd test-tar-checkout
assert_file_has_content hi hi
assert_file_has_content hello hi
assert_file_has_content subdir/more contents
assert_has_file subdir/1/empty
assert_has_file subdir/2/empty
cd ${test_tmpdir}
rm -rf test-tar-checkout
echo "ok tar contents"

cd ${test_tmpdir}
mkdir hardlinktest
cd hardlinktest
echo other > otherfile
echo foo1 > foo
ln foo bar
tar czf ${test_tmpdir}/hardlinktest.tar.gz .
cd ${test_tmpdir}
$OSTREE commit -s 'hardlinks' -b test-hardlinks --tar hardlinktest.tar.gz
rm -rf hardlinktest
echo "ok hardlink commit"

cd ${test_tmpdir}
$OSTREE checkout test-hardlinks test-hardlinks-checkout
cd test-hardlinks-checkout
assert_file_has_content foo foo1
assert_file_has_content bar foo1
echo "ok hardlink contents"
