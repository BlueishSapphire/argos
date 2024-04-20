#!/bin/env bash



# needs root to write to /usr/local/include/... and /usr/local/lib/...
[[ "$EUID" -ne 0 ]] && echo "must be run as root" && exit 1



####  CONFIG  ####

# target directory for the header files to be copied to
BIN_DIR="/usr/local/bin/"

# target directory for the final lib file to be copied to
MAN_DIR="/usr/local/man/man1/"

# project root, $(pwd) works as long as you run this script in the root directory of the project
PROJ_ROOT=$(pwd)

# filename for the final lib file
TARGET="argos"



####  LOGIC  ####

# make sure config settings are good
[[ -z "$PROJ_ROOT" ]] && echo "PROJ_ROOT is not set" && exit 1
[[ -z "$BIN_DIR" ]] && echo "BIN_DIR is not set" && exit 1
[[ -z "$MAN_DIR" ]] && echo "MAN_DIR is not set" && exit 1
[[ -z "$TARGET" ]] && echo "TARGET is not set" && exit 1

# if the build directory does not exist, create it
[[ ! -d "$PROJ_ROOT/build" ]] && mkdir "$PROJ_ROOT/build/"

# clear the build directory
rm -rf "$PROJ_ROOT/build/"*

# enter the build directory and compile all the sources into the target
cd "$PROJ_ROOT/build/" && gcc "$PROJ_ROOT/"*.c -lsoph -o "$TARGET"

# copy the bin file into $BIN_DIR
cp "$PROJ_ROOT/build/$TARGET" "$BIN_DIR"

# create a man file by running help2man on the executable
help2man "$PROJ_ROOT/build/$TARGET" > "$PROJ_ROOT/build/$TARGET.1"

# create a man file by running help2man on the executable
gzip "$PROJ_ROOT/build/$TARGET.1" -k

# copy the man file into $MAN_DIR
cp "$PROJ_ROOT/build/$TARGET.1.gz" "$MAN_DIR"

# remove the temporary build directory
#rm -rf "$PROJ_ROOT/build"

