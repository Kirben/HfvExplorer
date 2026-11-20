#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 NEW_FIXTURE_DIRECTORY" >&2
    exit 2
fi

mkdir "$1"
fixture_dir=$(cd "$1" && pwd)
source_dir=$(mktemp -d)
trap 'rm -f -- "$source_dir/sub/payload.txt"; rmdir -- "$source_dir/sub" "$source_dir"' EXIT
mkdir "$source_dir/sub"
printf 'UDF interoperability\n' > "$source_dir/sub/payload.txt"
"${GENISOIMAGE:-genisoimage}" -quiet -udf -R -V GENISOIMAGE \
    -o "$fixture_dir/geniso-bridge.iso" "$source_dir"

make_udf() {
    media=$1
    revision=$2
    filename=$3
    shift 3
    "${MKUDFFS:-mkudffs}" --new-file --blocksize=2048 --media-type="$media" \
        --udfrev="$revision" --label=OPTICAL "$@" "$fixture_dir/$filename.udf" 8192
}

make_udf cdr 1.50 cdr-1.50
make_udf cdr 2.01 cdr-2.01
make_udf cdr 2.01 cdr-closed-2.01 --closed
make_udf dvdr 2.01 dvdr-2.01
make_udf bdr 2.60 bdr-2.60
make_udf cdrw 2.01 cdrw-2.01
make_udf dvdrw 2.01 dvdrw-2.01
