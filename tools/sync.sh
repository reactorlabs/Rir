#!/bin/bash

set -e

CURRENT_DIR=`pwd`
SCRIPTPATH=`cd $(dirname "$0") && pwd`
if [ ! -d $SCRIPTPATH ]; then
    echo "Could not determine absolute dir of $0"
    echo "Maybe accessed with symlink"
fi
SRC_DIR=`cd ${SCRIPTPATH}/.. && pwd`
. "${SCRIPTPATH}/script_include.sh"


if [[ "$OSTYPE" == "darwin"* ]]; then
    USING_OSX=1
fi

if [[ "$1" == "--macos_gcc9" ]]; then
    MACOS_GCC9=1
fi

echo "-> update submodules"
git submodule update --init

# check the .git of the rjit directory
test -d ${SRC_DIR}/.git
IS_GIT_CHECKOUT=$?

if [ $IS_GIT_CHECKOUT -eq 0 ]; then
    echo "-> install git hooks"
    ${SRC_DIR}/tools/install_hooks.sh
fi

function build_r {
    NAME=$1
    R_DIR="${SRC_DIR}/external/${NAME}"

    cd $R_DIR

    if [[ $(git diff --shortstat 2> /dev/null | tail -n1) != "" ]]; then
        echo "** warning: $NAME repo is dirty"
        sleep 1
    fi

    # unpack cache of recommended packages
    cd src/library/Recommended/
    tar xf ../../../../custom-r/cache_recommended.tar
    cd ../../..
    tools/rsync-recommended || true

    # There is a test that times out due to the compiler triggering in the
    # wrong moment in the matrix package. There doesn't seem to be a good solution
    # other than just patching it.
    cd src/library/Recommended
    tar xzf Matrix_1.2-18.tar.gz
    sed -i -e 's/^stopifnot((st <- system.time(show(M)))\[1\] < 1.0)/((st <- system.time(show(M)))[1] < 1.0);((st <- system.time(show(M)))[1] < 1.0);((st <- system.time(show(M)))[1] < 1.0);stopifnot((st <-  system.time(show(M)))[1] < 1.0)/' Matrix/man/printSpMatrix.Rd
    rm Matrix_1.2-18.tar.gz
    tar czf Matrix_1.2-18.tar.gz Matrix
    rm -rf Matrix
    cd ../../../

    if [ ! -f $R_DIR/Makefile ]; then
        echo "-> configure $NAME"
        cd $R_DIR
        if [ $USING_OSX -eq 1 ]; then
            # Mac OSX
            if [ $MACOS_GCC9 -eq 1 ]; then
                MACOS_GCC9=1 ./configure --enable-R-shlib --with-internal-tzcode --with-ICU=no --without-aqua || cat config.log
            else
                ./configure --enable-R-shlib --with-internal-tzcode --with-ICU=no || cat config.log
            fi
        else
            ./configure --with-ICU=no
        fi
    fi

    if [ ! -f $R_DIR/doc/FAQ ]; then
        cd $R_DIR
        touch doc/FAQ
    fi

    if [ ! -f $R_DIR/SVN-REVISION ]; then
        # R must either be built from a svn checkout, or from the tarball generated by make dist
        # this is a workaround to build it from a git mirror
        # see https://github.com/wch/r-source/wiki/Home/6d35777dcb772f86371bf221c194ca0aa7874016#building-r-from-source
        echo -n 'Revision: ' > SVN-REVISION
        # get the latest revision that is not a rir patch
        REV=$(git log --grep "git-svn-id" -1 --format=%B | grep "^git-svn-id" | sed -E 's/^git-svn-id: https:\/\/svn.r-project.org\/R\/[^@]*@([0-9]+).*$/\1/')
        # can fail on shallow checkouts, so let's put the last known there
        if [ "$REV" == "" ]; then
          REV='74948'
        fi
        echo $REV >> SVN-REVISION
        echo -n 'Last Changed Date: ' >> SVN-REVISION
        REV_DATE=$(git log --grep "git-svn-id" -1 --pretty=format:"%ad" --date=iso | cut -d' ' -f1)
        # can fail on shallow checkouts, so let's put the last known there
        if [ "$REV_DATE" == "" ]; then
          REV_DATE='2018-07-02'
        fi
        echo $REV_DATE >> SVN-REVISION

        rm -f non-tarball
    fi

    echo "-> building $NAME"
    make
}

build_r custom-r

LLVM_DIR="${SRC_DIR}/external/llvm-8.0.0"
if [ ! -d $LLVM_DIR ]; then
    echo "-> unpacking LLVM"
    cd "${SRC_DIR}/external"
    if [ $USING_OSX -eq 1 ]; then
        if [ ! -f "clang+llvm-8.0.0-x86_64-apple-darwin.tar.xz" ]; then
            curl http://releases.llvm.org/8.0.0/clang+llvm-8.0.0-x86_64-apple-darwin.tar.xz > clang+llvm-8.0.0-x86_64-apple-darwin.tar.xz
        fi
        tar xf clang+llvm-8.0.0-x86_64-apple-darwin.tar.xz
        mv clang+llvm-8.0.0-x86_64-apple-darwin llvm-8.0.0
    else
        if [ ! -f "clang+llvm-8.0.0-x86_64-linux-gnu-ubuntu-18.04.tar.xz" ]; then
            curl http://releases.llvm.org/8.0.0/clang+llvm-8.0.0-x86_64-linux-gnu-ubuntu-18.04.tar.xz > clang+llvm-8.0.0-x86_64-linux-gnu-ubuntu-18.04.tar.xz
        fi
        tar xf clang+llvm-8.0.0-x86_64-linux-gnu-ubuntu-18.04.tar.xz
        mv clang+llvm-8.0.0-x86_64-linux-gnu-ubuntu-18.04 llvm-8.0.0
    fi
fi
