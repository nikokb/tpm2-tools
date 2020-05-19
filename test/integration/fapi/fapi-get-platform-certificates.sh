#!/bin/bash

set -e
source helpers.sh

start_up

setup_fapi

PATH=${BUILDDIR}/tools/fapi:$PATH

function cleanup {
    tss2_delete --path /
    shut_down
}

trap cleanup EXIT

CERTIFICATES_OUTPUT_FILE=$TEMP_DIR/certificates_output.file

tss2_provision

expect <<EOF
# Try with missing certificates
spawn tss2_getplatformcertificates
set ret [wait]
if {[lindex \$ret 2] || [lindex \$ret 3] != 1} {
    Command has not failed as expected\n"
    exit 1
}
EOF

expect <<EOF
# Try normal command; should fail since no certificates present
spawn tss2_getplatformcertificates --certificates $CERTIFICATES_OUTPUT_FILE \
    --force
set ret [wait]
if {[lindex \$ret 2] || [lindex \$ret 3] != 1} {
    Command has not failed as expected\n"
    exit 1
}
EOF

exit 0