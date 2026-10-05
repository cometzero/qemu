# Default configuration for tricore-softmmu

# This downstream Hexagon board selects Arm-compatible semihosting hooks,
# which the TriCore target does not provide.
CONFIG_DRAGON=n

# Boards are selected by default, uncomment to keep out of the build.
# CONFIG_TRICORE_TESTBOARD=n
# CONFIG_TRIBOARD=n
# CONFIG_TC397=n
