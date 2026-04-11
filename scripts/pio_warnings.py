Import("env")

# Sqlite3Esp32 ships sqlite3.c that triggers -Wdiscarded-qualifiers on GCC C builds.
# Apply this warning override to C compilation only (does not touch C++ diagnostics).
env.AppendUnique(CFLAGS=["-Wno-discarded-qualifiers"])
