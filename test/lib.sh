#!/bin/sh
# Helper functions for testing SMCRoute

# Test name, used everywhere as /tmp/$NM/foo
NM=$(basename "$0" .sh)

# Print heading for test phases
print()
{
    printf "\e[7m>> %-80s\e[0m\n" "$1"
}

show_mroute()
{
    # Show active routes (and counters)
    cat /proc/net/ip_mr_cache
    cat /proc/net/ip6_mr_cache
    echo "-----------------------------------------------------------------------------------"
    ip mroute
    ip -6 mroute
    echo "-----------------------------------------------------------------------------------"
    ../src/smcroutectl -pd -S "/tmp/$NM/sock"
}

collect()
{
    print "Starting collector ..."
    tshark -w "/tmp/$NM/pcap" -lni "$@" 2>/dev/null &
    echo $! >> "/tmp/$NM/PIDs"
    sleep 1
}

# Set up a basic bridge topology, two VETH pairs with one end in the
# bridge and the other free.  Each pair is also in a separate VLAN.
#
# No IP address assignment is done in topo files, only topology setup.
#
# Topology:          ¦
#             vlan1  ¦  vlan2
#                  \ ¦ /
#       a1 -------- br0 --------- a2
#                    ¦
#       VLAN 1       ¦        VLAN 2
#
# Note: in addition to VLAN filtering, the bridge has both IGMP and MLD
#       snooping disabled, because the main purpose of these tests is to
#       verify the IPv4 and IPv6 routing functionality of SMCRoute.
#       Future tests may include verifying join/leave of groups (TODO)
topo_bridge()
{
    cat << EOF > "$NM-topo.ip"
link add br0 type bridge vlan_filtering 1 mcast_snooping 0
link add a1 type veth peer b1
link add a2 type veth peer b2
link set b1 master br0
link set b2 master br0

link set a1 up
link set b1 up
link set a2 up
link set b2 up
link set br0 up

link add link br0 vlan1 type vlan id 1
link add link br0 vlan2 type vlan id 2

link set vlan1 up
link set vlan2 up
EOF

    # Move b2 to VLAN 2
    # Set br0 as tagged member of both VLANs
    cat <<EOF > "$NM-bridge.ip"
vlan add vid 2 dev b2 pvid untagged
vlan del vid 1 dev b2

vlan add vid 1 dev br0 self
vlan add vid 2 dev br0 self
EOF

    ip     -force -batch "$NM-topo.ip"
    bridge -force -batch "$NM-bridge.ip"

    rm -f "$NM-topo.ip" "$NM-bridge.ip"
}


# Set up a basic dummy interface topology,
#
# No IP address assignment is done in topo files, only topology setup.
topo_basic()
{
    cat << EOF > "$NM-topo.ip"
link add a1 type dummy
link set a1 up
link set a1 multicast on

link add a2 type dummy
link set a2 up
link set a2 multicast on
EOF

    ip -force -batch "$NM-topo.ip"
    rm -f "$NM-topo.ip"

    return 2
}

# Same as basic topology, but with multiple inbound interfaces.
#
# No IP address assignment is done in topo files, only topology setup.
topo_multi()
{
    cat << EOF > "$NM-topo.ip"
link add a1 type dummy
link set a1 up
link set a1 multicast on

link add a2 type dummy
link set a2 up
link set a2 multicast on

link add a3 type dummy
link set a3 up
link set a3 multicast on

link add a4 type dummy
link set a4 up
link set a4 multicast on

link add b1 type dummy
link set b1 up
link set b1 multicast on

link add b2 type dummy
link set b2 up
link set b2 multicast on

link add b3 type dummy
link set b3 up
link set b3 multicast on

link add b4 type dummy
link set b4 up
link set b4 multicast on
EOF

    ip -force -batch "$NM-topo.ip"
    rm -f "$NM-topo.ip"
}

# Set up VLAN interfaces on top of dummy interfaces
# shellcheck disable=SC2048
topo_basic_vlan()
{
    num=$1
    shift

    i=1
    while [ $i -le "$num" ]; do
	iface=a$i
	i=$((i + 1))

	for vid in $*; do
	    ip link add "$iface.$vid" link $iface type vlan id "$vid"
	    ip link set "$iface.$vid" up
	    ip link set "$iface.$vid" multicast on
	done
    done
}

topo_isolated()
{
    left="$1"
    right="$2"
    lif=$(basename "$left")
    rif=$(basename "$right")

    touch "$left" "$right"
    PID=$$

    echo "$left"   > "/tmp/$NM/mounts"
    echo "$right" >> "/tmp/$NM/mounts"

    unshare --net="$left" -- ip link set lo up
    nsenter --net="$left" -- ip link add eth0 type veth peer "$lif"
    nsenter --net="$left" -- ip link set "$lif" netns $PID
    nsenter --net="$left" -- ip link set eth0 up
    ip link set "$lif" up

    unshare --net="$right" -- ip link set lo up
    nsenter --net="$right" -- ip link add eth0 type veth peer "$rif"
    nsenter --net="$right" -- ip link set "$rif" netns $PID
    nsenter --net="$right" -- ip link set eth0 up
    ip link set "$rif" up
}

# Same as bridge topology, but with the VETH endpoints constructed
# by the isolated topology.  We just rename the main namespace's
# bridge ports to match.
topo_isolated_bridge()
{
    left="$1"
    right="$2"
    lif=$(basename "$left")
    rif=$(basename "$right")

    topo_isolated "$@"

    # This will fail to create the VETH pairs, but that's OK since
    # we've set up the b1 and b2 interfaces here to be enslaved in
    # the bridge instead.
    #topo_bridge
    echo "Creating br0, adding $lif and $rif as bridge ports"
    ip link add br0 type bridge vlan_filtering 1 mcast_snooping 0
    ip link set "$lif" master br0
    ip link set "$rif" master br0

    ip link set br0 up

    ip link add link br0 vlan1 type vlan id 1
    ip link add link br0 vlan2 type vlan id 2

    ip link set vlan1 up
    ip link set vlan2 up

    bridge vlan add vid 2 dev "$rif" pvid untagged
    bridge vlan del vid 1 dev "$rif"

    bridge vlan add vid 1 dev br0 self
    bridge vlan add vid 2 dev br0 self
}

topo_teardown()
{
    if [ -z "$NM" ]; then
	echo "NM variable unset, skippnig teardown"
	exit 1
    fi

    if [ -f "/tmp/$NM/pid" ]; then
	PID=$(cat "/tmp/$NM/pid")
	kill -9 "$PID"
    fi

    # shellcheck disable=SC2162
    if [ -f "/tmp/$NM/mounts" ]; then
        while read ln; do umount "$ln"; rm "$ln"; done < "/tmp/$NM/mounts"
    fi

    # shellcheck disable=SC2162
    if [ -f "/tmp/$NM/PIDs" ]; then
	while read ln; do kill "$ln" 2>/dev/null; done < "/tmp/$NM/PIDs"
    fi

    ip link del br0  2>/dev/null
    ip link del a1   2>/dev/null
    ip link del a2   2>/dev/null
    ip link del b1   2>/dev/null
    ip link del b2   2>/dev/null

    rm -rf "/tmp/$NM"
}

signal()
{
    echo
    print "Got signal, cleaning up"
    topo_teardown
    exit 1
}

topo()
{
    if [ ! -d "/tmp/$NM" ]; then
	mkdir "/tmp/$NM"
	touch "/tmp/$NM/PIDs"
	trap signal INT TERM QUIT
    fi

    if [ $# -lt 1 ]; then
	print "Too few arguments to topo()"
	exit 1
    fi
    t=$1
    shift

    case "$t" in
	bridge)
	    topo_bridge
	    ;;

	basic)
	    topo_basic
	    num=$?
	    case "$1" in
		vlan)
		    shift
		    topo_basic_vlan $num "$@"
		    ;;
	    esac
	    ;;

	isolated)
	    case "$1" in
		bridge)
		    shift
		    topo_isolated_bridge "$@"
		    ;;
		*)
	    	    topo_isolated "$@"
		    ;;
	    esac
	    ;;

	multi)
	    topo_multi
	    ;;

	teardown)
	    topo_teardown
	    ;;
	*)
	    print "No such topology: $1"
	    exit 1
	    ;;
    esac
}
