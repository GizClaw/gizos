img=$1 layout=$2
shift 2
mkdir input empty-root output
for part do cp "$part" input/; done
# FAT creates directory-entry timestamps independently of kernel and SquashFS metadata.
touch -d "@$SOURCE_DATE_EPOCH" input/*
faketime -f "$(date -u -d "@$SOURCE_DATE_EPOCH" '+%F %T')" genimage \
    --config "$layout" --rootpath empty-root --tmppath gen-tmp --inputpath input --outputpath output
cp --sparse=always output/sdcard.img "$img"
