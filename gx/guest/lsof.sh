#!/bin/sh
# lsof.sh - /usr/bin/lsof in the root of a Steam client under gx.
#
# The client lets nothing connect to the port its interface talks to it by
# but its own browser, and asks lsof who holds the other end of each
# connection: "lsof -P -F upnR -i TCP@127.0.0.1:<port>". With no lsof, or one
# that cannot tell, the connection is "rejected", the interface has nobody to
# talk to ("Transport SendMsg: socket not ready", error 0x3008), and asks
# again for ever with what it meant to send piling up.
#
# No lsof could tell here: the kernel knows the guest's sockets by its own
# threads, not by the guest's processes. But only the browser connects there,
# so that is the answer: gx says which process it is (/proc/gx/of/...), as
# lsof would. Nothing but the shell's own words: a program started from here
# is a process made in the client's memory while it waits.
port=${*##*:}
while read -r line; do
	echo "$line"
done < /proc/gx/of/steamwebhelper || exit 1
echo "f0"
echo "n127.0.0.1:$port->127.0.0.1:0"
