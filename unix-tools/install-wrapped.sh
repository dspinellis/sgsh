#!/bin/sh
#
# Install the commands wrapped by dgsh-wrap
#

if [ "$1" = "" ] ; then
	DGPATH=$DESTDIR$PREFIX/libexec/dgsh
	mkdir -p $DGPATH
else
	DGPATH=$1/libexec/dgsh
	mkdir -p $DGPATH
fi

# Remove comments and blank lines
sed 's/[ \t]*#.*//;/^$/d' wrapped-commands-posix wrapped-commands-tests |
while read mode name ; do
  # Continue if command is not available
  if ! which $name 2>/dev/null >/dev/null ; then
    continue
  fi

  # Continue if command is custom-implemented or both deaf and mute
  if [ $mode = c -o $mode = dm ] ; then
    continue
  fi

  # Iterate over the mode's characters creating $opt
  opt=' -s'
  for m in $(echo "$mode" | sed 's/./& /g') ; do
    case $m in
      m)	# Mute
	opt="$opt -o 0"
	;;
      M)	# Mute unless - is specified (TODO)
	;;
      d)	# Deaf
	opt="$opt -i 0"
	;;
      I)	# Count stdin in channel assignments
	opt="$opt -I"
	;;
      D)	# Deaf unless - is specified or no arguments are provided (TODO)
	;;
      f)	# Filter
	;;
      *)
	echo "Unknown I/O mode character $m for $name" 1>&2
	exit 1
    esac
  done
  target=$DGPATH/$name
  echo "#!$PREFIX/libexec/dgsh/dgsh-wrap$opt" >$target
  chmod 755 $target
done
