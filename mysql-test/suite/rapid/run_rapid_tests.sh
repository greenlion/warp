#!/bin/bash
#
# Wrapper script to run RAPID test suite with required LD_PRELOAD
#
# Usage:
#   ./run_rapid_tests.sh              # Run all RAPID tests
#   ./run_rapid_tests.sh basic        # Run specific test
#   ./run_rapid_tests.sh --record     # Record test results
#

# Source directory (where this script lives)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_MYSQL_TEST_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Build directory where mtr and plugins are located
BUILD_DIR="${BUILD_DIR:-/home/ubuntu/mysql/8.4}"
MTR="${BUILD_DIR}/mysql-test/mtr"
PLUGIN_DIR="${PLUGIN_DIR:-${BUILD_DIR}/lib/plugin}"
BUILD_SUITE_DIR="${BUILD_DIR}/mysql-test/suite"

if [ ! -f "${PLUGIN_DIR}/ha_rapid.so" ]; then
    echo "Error: ha_rapid.so not found in ${PLUGIN_DIR}"
    echo "Please build the RAPID plugin first: make rapid"
    exit 1
fi

if [ ! -f "${MTR}" ]; then
    echo "Error: mtr not found at ${MTR}"
    echo "Please ensure BUILD_DIR is set correctly"
    exit 1
fi

# Create suite directory in build dir if it doesn't exist
if [ ! -d "${BUILD_SUITE_DIR}" ]; then
    mkdir -p "${BUILD_SUITE_DIR}"
fi

# Create symlink to rapid test suite in build directory
if [ ! -e "${BUILD_SUITE_DIR}/rapid" ]; then
    echo "Creating symlink: ${BUILD_SUITE_DIR}/rapid -> ${SCRIPT_DIR}"
    ln -s "${SCRIPT_DIR}" "${BUILD_SUITE_DIR}/rapid"
fi

# Build LD_PRELOAD value for mysqld
LD_PRELOAD_VALUE="${PLUGIN_DIR}/ha_rapid.so"

echo "Running RAPID tests with --mysqld-env LD_PRELOAD=${LD_PRELOAD_VALUE}"
echo ""

cd "${BUILD_DIR}/mysql-test"

# Build argument list
ARGS=("--mysqld-env" "LD_PRELOAD=${LD_PRELOAD_VALUE}" "--suite=rapid")

# Collect test names and other options
TEST_NAMES=()
for arg in "$@"; do
    case "$arg" in
        --*)
            # Pass options as-is
            ARGS+=("$arg")
            ;;
        *)
            # Collect test names (without suite prefix)
            TEST_NAMES+=("$arg")
            ;;
    esac
done

# If specific tests were requested, add them
if [ ${#TEST_NAMES[@]} -gt 0 ]; then
    for test in "${TEST_NAMES[@]}"; do
        ARGS+=("$test")
    done
fi

exec "${MTR}" "${ARGS[@]}"
