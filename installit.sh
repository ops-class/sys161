#!/bin/sh
#
# installit.sh - install a System/161 binary
#
# usage: installit.sh installdir prog version
#

INSTALLDIR="$1"
PROG="$2"
VERSION="$3"

if [ "x$4" != x ]; then
    echo "Usage: $0 installdir prog version" 1>&2
    exit 1
fi

############################################################
#
# cross-check arguments
#

if [ ! -d "$INSTALLDIR" ]; then
    # should already have been created
    echo "$0: Invalid/missing installation directory $INSTALLDIR" 1>&2
    exit 1
fi

case "$PROG" in
    sys161|trace161|stat161|hub161|disk161) ;;
    *)
	echo "$0: Invalid program $PROG" 1>&2
	exit 1
    ;;
esac

case "$VERSION" in
    devel) ;;				# devel version out of cvs
    201[5-9][01][0-9][0-3][0-9]) ;;	# snap
    [0-9].99.[0-9][0-9]) ;;		# devel release
    [0-9].99.[0-9]) ;;			# devel release
    [0-9].[0-9].[0-9][0-9])	 ;;	# release
    [0-9].[0-9].[0-9]) ;;		# release
    [0-9].[0-9]) ;;			# release
    *)
	echo "$0: Invalid version $VERSION" 1>&2
	exit 1
    ;;
esac

############################################################
#
# Do it.
#

VTARG="${INSTALLDIR}/${PROG}-${VERSION}"
TARG="${INSTALLDIR}/${PROG}"

#
# Copy the file.
#
cp "${PROG}" "${VTARG}.new" || exit 1
chmod 755 "${VTARG}.new"

#
# We might be installing the same version a second time, particularly
# if the version is "devel". Move the old one out of the way to avoid
# ETXTBUSY on some systems if it's running. But suppress any errors.
#
mv -f "${VTARG}" "${VTARG}.old" >/dev/null 2>&1

#
# Put the new one in place.
#
mv -f "${VTARG}.new" "${VTARG}" || exit 1

#
# Now update the symbolic links.
# We want sys161.old to point to the previous installed *version*, even if
# we reinstall the same version several times. So do some silly checks.
#

SKIP=0
if [ -h "${TARG}" -a -h "${TARG}.old" ]; then
    PREVLINK=`ls -l ${TARG} | awk '{ print $NF }'`
    if [ "x$PREVLINK" = "x${PROG}-${VERSION}" ]; then
	SKIP=1
    fi
fi

if [ $SKIP = 0 ]; then
    rm -f "${TARG}.old" >/dev/null 2>&1
    mv -f "${TARG}" "${TARG}.old" >/dev/null 2>&1
    ln -s "${PROG}-${VERSION}" "${TARG}" || exit 1
fi

