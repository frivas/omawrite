# The version, and the commit the binary was built from, for About to name.
# Shared by the app and the tests so the two cannot drift: a test that asserts
# the version has to be built with the same definition of it.
VERSION = 0.1.0

OMAWRITE_COMMIT = $$system(git -C $$PWD rev-parse --short HEAD 2>/dev/null)
isEmpty(OMAWRITE_COMMIT): OMAWRITE_COMMIT = unknown
else {
    OMAWRITE_DIRTY = $$system(git -C $$PWD status --porcelain 2>/dev/null)
    !isEmpty(OMAWRITE_DIRTY): OMAWRITE_COMMIT = $${OMAWRITE_COMMIT}+
}

DEFINES += OMAWRITE_VERSION=\\\"$$VERSION\\\"
DEFINES += OMAWRITE_COMMIT=\\\"$$OMAWRITE_COMMIT\\\"
