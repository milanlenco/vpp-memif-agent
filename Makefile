all:
	gcc -I/home/mlenco/dev/vpp-github/build-root/install-vpp_lite_debug-native/vpp/include \
	    -I/home/mlenco/dev/vpp-github/build-root/install-vpp_lite_debug-native/vpp/include/vpp_plugins \
	    -L/home/mlenco/dev/vpp-github/build-root/install-vpp_lite_debug-native/vpp/lib64/ \
	    -Wall -std=gnu99 -g -O0 \
	    memif_agent.c \
	    -lvlibmemoryclient -lvlibapi \
	    -o memif_agent
