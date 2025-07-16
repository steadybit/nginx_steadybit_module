#!/bin/bash
#
# Copyright 2025 steadybit GmbH. All rights reserved.
#

#
# Test script for ngx_steadybit_sleep_module.c
# This script builds and tests the sleep module locally
#

set -e  # Exit on error

# Configuration
NGINX_VERSION="1.27.4"
TEST_PORT=8888  # Default port for testing, can be overridden if already in use
TEST_DIR="/tmp/nginx-delay-test"

echo "=== Testing ngx_steadybit_sleep_module locally ==="
echo "Using NGINX version: $NGINX_VERSION"
echo "Using port: $TEST_PORT"

#Delete previous test directory if it exists
if [ -d "$TEST_DIR" ]; then
    echo "Removing existing test directory: $TEST_DIR"
    rm -rf $TEST_DIR
fi

# Create test directory
mkdir -p $TEST_DIR
# remember current directory
ORIGINAL_DIR=$(pwd)
cd $TEST_DIR

# Check if port is already in use
if lsof -i :$TEST_PORT > /dev/null; then
    echo "⚠️ Port $TEST_PORT is already in use. Trying to find an available port..."
    for port in 8889 8890 8891 8892 8893; do
        if ! lsof -i :$port > /dev/null; then
            TEST_PORT=$port
            echo "Found available port: $TEST_PORT"
            break
        fi
    done
    if lsof -i :$TEST_PORT > /dev/null; then
        echo "❌ Could not find an available port. Please manually specify a different port."
        exit 1
    fi
fi

echo "Creating test directory at $TEST_DIR"

# Download and extract Nginx
if [ ! -f "nginx-$NGINX_VERSION.tar.gz" ]; then
  echo "Downloading Nginx $NGINX_VERSION..."
  curl -s -O "https://nginx.org/download/nginx-$NGINX_VERSION.tar.gz"
fi

if [ ! -d "nginx-$NGINX_VERSION" ]; then
  echo "Extracting Nginx..."
  tar -xzf "nginx-$NGINX_VERSION.tar.gz"
fi

# Copy the module source
echo "Copying sleep module source..."
cp $ORIGINAL_DIR/ngx_steadybit_sleep_module.c $TEST_DIR/

# Create module config
echo "Creating module config..."
mkdir -p $TEST_DIR/sleep
cp $TEST_DIR/ngx_steadybit_sleep_module.c $TEST_DIR/sleep/
cat > $TEST_DIR/sleep/config << 'EOF'
ngx_addon_name=ngx_steadybit_sleep_module
if test -n "$dynamic_modules" || test -n "$ngx_module_link"; then
    ngx_module_type=HTTP
    ngx_module_name=ngx_steadybit_sleep_module
    ngx_module_srcs="$ngx_addon_dir/ngx_steadybit_sleep_module.c"
    ngx_module_libs=
    . auto/module
else
    HTTP_MODULES="$HTTP_MODULES ngx_steadybit_sleep_module"
    NGX_ADDON_SRCS="$NGX_ADDON_SRCS $ngx_addon_dir/ngx_steadybit_sleep_module.c"
fi
EOF

# Build the module and Nginx
cd $TEST_DIR/nginx-$NGINX_VERSION
echo "Configuring Nginx with sleep module..."
./configure \
  --prefix=$TEST_DIR/nginx \
  --with-compat \
  --with-debug \
  --add-dynamic-module=../sleep

echo "Building Nginx and modules..."
make
make install

# The path to the compiled Nginx binary
NGINX_BIN=$TEST_DIR/nginx/sbin/nginx

# Create a test Nginx configuration
echo "Creating test Nginx configuration..."
mkdir -p $TEST_DIR/nginx/logs
mkdir -p $TEST_DIR/nginx/conf
mkdir -p $TEST_DIR/nginx/modules
mkdir -p $TEST_DIR/nginx/html

# Create a simple index.html
echo "Creating test HTML files..."
echo "No sleep test page" > $TEST_DIR/nginx/html/index.html

# Copy the compiled module
cp $TEST_DIR/nginx-$NGINX_VERSION/objs/ngx_steadybit_sleep_module.so $TEST_DIR/nginx/modules/

# Check if module was actually built
echo "Checking if module was built..."
if [ ! -f "$TEST_DIR/nginx/modules/ngx_steadybit_sleep_module.so" ]; then
    echo "❌ ERROR: Module file not found at $TEST_DIR/nginx/modules/ngx_steadybit_sleep_module.so"
    ls -la $TEST_DIR/nginx-$NGINX_VERSION/objs/
    echo "Looking for .so files in build directory:"
    find $TEST_DIR/nginx-$NGINX_VERSION -name "*.so"
    exit 1
fi

# Check module info
echo "Checking module file info:"
file $TEST_DIR/nginx/modules/ngx_steadybit_sleep_module.so

# Try to check symbols, but don't fail if it doesn't work
echo "Attempting to check module symbols (may not work on all platforms):"
nm -D $TEST_DIR/nginx/modules/ngx_steadybit_sleep_module.so 2>/dev/null | grep -i sleep || \
nm $TEST_DIR/nginx/modules/ngx_steadybit_sleep_module.so 2>/dev/null | grep -i sleep || \
echo "Could not extract symbols from module (this is normal on some platforms)"

# Create nginx.conf with the simplest possible configuration
cat > $TEST_DIR/nginx/conf/nginx.conf << EOF
load_module $TEST_DIR/nginx/modules/ngx_steadybit_sleep_module.so;

worker_processes 1;
daemon off;
master_process off;
error_log $TEST_DIR/nginx/logs/error.log debug;
pid $TEST_DIR/nginx/logs/nginx.pid;

events {
    worker_connections 128;
}

http {
    default_type text/plain;

    log_format main '\$remote_addr - \$remote_user [\$time_local] "\$request" '
                    '\$status \$body_bytes_sent "\$http_referer" '
                    '"\$http_user_agent" "\$http_x_forwarded_for" '
                    'rt=\$request_time';

    access_log $TEST_DIR/nginx/logs/access.log main;

    server {
        listen $TEST_PORT;
        server_name localhost;

        set \$sleep_ms_duration 0;
        if (\$request_uri ~* /server-delay-test) {
            set \$sleep_ms_duration 300;
        }

        sb_sleep_ms \$sleep_ms_duration;

        # Root location - no sleep
        location = / {
            add_header Content-Type text/plain;
            return 200 "No sleep\n";
        }

        # Test with 100ms sleep
        location = /sleep-100ms {
            sb_sleep_ms 100;
            proxy_pass http://localhost:9000/;
        }

        # Test with 500ms sleep
        location = /sleep-500ms {
            sb_sleep_ms 500;
            proxy_pass http://localhost:9000/;
        }

        # Test with 1s sleep
        location = /sleep-1s {
            sb_sleep_ms 1000;
            proxy_pass http://localhost:9000/;
        }

        location = /server-delay-test {
            proxy_pass http://localhost:9000/;
        }
    }
}
EOF

# Validate the configuration
echo "Validating Nginx configuration..."
$NGINX_BIN -c $TEST_DIR/nginx/conf/nginx.conf -t

# Run Nginx in the background
echo "Starting Nginx with sleep module on port $TEST_PORT..."
$NGINX_BIN -c $TEST_DIR/nginx/conf/nginx.conf &
NGINX_PID=$!

# Wait for Nginx to start
echo "Waiting for Nginx to start..."
sleep 2

# Check if Nginx is actually running
if ! kill -0 $NGINX_PID 2>/dev/null; then
    echo "❌ ERROR: Nginx failed to start. Check error log at $TEST_DIR/nginx/logs/error.log"
    cat $TEST_DIR/nginx/logs/error.log
    exit 1
fi

# Print info about Nginx process
echo "Nginx process info:"
ps -p $NGINX_PID -o pid,ppid,command

# Check if the server is responding at all
echo "Testing basic connectivity..."
curl -s "http://localhost:$TEST_PORT/" || echo "Failed to connect to Nginx server!"

# Function to test endpoint and measure time
test_endpoint() {
    endpoint=$1
    expected_time=$2

    echo ""
    echo "Testing $endpoint (expected ~$expected_time ms)..."
    echo "Command: curl -v http://localhost:$TEST_PORT$endpoint"

    # First get verbose output to debug connectivity
    curl -v "http://localhost:$TEST_PORT$endpoint" 2>&1 | head -n 20

    # Then measure timing with multiple requests to get a better average
    total_duration=0
    num_requests=3

    for i in $(seq 1 $num_requests); do
        start_time=$(date +%s.%N)
        response=$(curl -s "http://localhost:$TEST_PORT$endpoint")
        end_time=$(date +%s.%N)

        duration=$(echo "($end_time - $start_time) * 1000" | bc)
        total_duration=$(echo "$total_duration + $duration" | bc)
        echo "Request $i took $duration ms"
    done

    avg_duration=$(echo "scale=2; $total_duration / $num_requests" | bc)
    echo "Response: $response"
    echo "Average request took $avg_duration ms"

    # Simple validation
    if [ $(echo "$avg_duration >= $expected_time * 0.8" | bc) -eq 1 ]; then
        echo "✅ Test passed! Average duration ($avg_duration ms) is at least 80% of expected time ($expected_time ms)"
    else
        echo "❌ Test failed! Average duration ($avg_duration ms) is less than 80% of expected time ($expected_time ms)"
        FAILED=1
    fi
}

# Track overall test status
FAILED=0

# Run tests
echo ""
echo "=== Running tests ==="
test_endpoint "/" 0
test_endpoint "/sleep-100ms" 100
test_endpoint "/sleep-500ms" 500
test_endpoint "/sleep-1s" 1000
test_endpoint "/server-delay-test" 300

# Check Nginx logs
echo ""
echo "=== Nginx error log ==="
tail -n 20 $TEST_DIR/nginx/logs/error.log

echo ""
echo "=== Nginx access log ==="
cat $TEST_DIR/nginx/logs/access.log

# Stop Nginx
echo ""
echo "Stopping Nginx..."
kill $NGINX_PID

echo ""
echo "=== Test completed ==="
echo "Test directory: $TEST_DIR"
echo "You can review the logs at:"
echo "- Access log: $TEST_DIR/nginx/logs/access.log"
echo "- Error log: $TEST_DIR/nginx/logs/error.log"

# Exit with proper code for CI
if [ $FAILED -eq 0 ]; then
    echo "All tests passed."
else
    echo "Some tests failed."
fi
exit $FAILED
