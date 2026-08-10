Import("env")

# Avoid loading an unrelated MinGW runtime from PATH on Windows test hosts.
# The firmware itself is unaffected; this script belongs only to [env:native].
env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
