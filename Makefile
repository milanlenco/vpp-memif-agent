all:
	gcc -I${VPP_REPO}/build-root/install-vpp_lite_debug-native/vpp/include \
	    -I${VPP_REPO}/build-root/install-vpp_lite_debug-native/vpp/include/vpp_plugins \
	    -L${VPP_REPO}/build-root/install-vpp_lite_debug-native/vpp/lib64/ \
	    -Wall -std=gnu99 -g -O0 \
	    memif_agent.c \
	    -lvlibmemoryclient -lvlibapi \
	    -o memif_agent
