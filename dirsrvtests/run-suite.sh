#!/bin/bash
# Run a single 389-ds-base pytest suite.
# Usage: run-suite.sh <suite-path>
#
# Environment variables:
#   NSSLAPD_DB_LIB - Database backend (bdb or mdb)
#   GSSAPI_ACK     - Acknowledge GSSAPI usage (set to 1)

set -ex

SUITE="$1"

if [ -z "$SUITE" ]; then
    echo "ERROR: No suite specified"
    exit 1
fi

SUITE_PATH="${TMT_TREE}/dirsrvtests/tests/suites/${SUITE}"

if [ ! -e "$SUITE_PATH" ]; then
    echo "ERROR: Suite path does not exist: $SUITE_PATH"
    exit 1
fi

cd "${TMT_TREE}"

py.test \
    --junit-xml="${TMT_TEST_DATA}/junit.xml" \
    -v \
    "$SUITE_PATH"
