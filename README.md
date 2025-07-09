# nginx_steadybit_module

## Overview

`ngx_steadybit_sleep_module` is a third-party NGINX module that allows you to introduce artificial delays (sleep) into HTTP request processing. This is useful for testing, chaos engineering, and simulating network latency in development or staging environments.

## Features
- Add a configurable delay to HTTP requests using a custom directive
- Supports millisecond granularity
- Designed for use with NGINX and NGINX Ingress (Red Hat UBI)

## Requirements
- NGINX (built from source, or compatible with dynamic modules)
- Compatible with NGINX Ingress images based on Red Hat UBI (e.g., `nginx/nginx-ingress:5.0.0-ubi`)
- GCC, make, and standard build tools (for building the module)

## Build Instructions

### Using Makefile (local build)
```sh
make
# The built module will be in dist/ngx_steadybit_sleep_module.so
```

### Using Docker (recommended for compatibility)
```sh
docker build -f Dockerfile.runtime -t steadybit/nginx-sleep-module:latest .
```

## Usage

1. **Copy the built `.so` file** to your NGINX modules directory (e.g., `/usr/lib/nginx/modules/`).
2. **Load the module** in your `nginx.conf`:
   ```nginx
   load_module modules/ngx_steadybit_sleep_module.so;
   ```
3. **Use the directive** in your server/location block:
   ```nginx
   location /test-sleep {
       sb_sleep_ms 500;  # Sleep for 500 milliseconds
       proxy_pass http://backend;
   }
   ```

## Configuration Example
```nginx
load_module modules/ngx_steadybit_sleep_module.so;

http {
    server {
        listen 8080;
        location /sleep {
            sb_sleep_ms 1000;
            return 200 'Slept for 1 second';
        }
    }
}
```

## Testing

A test script is provided:
```sh
cd ngx_steadybit_sleep_module
bash test-sleep-module.sh
```
This will build NGINX with the module, start a test server, and run latency tests against the endpoints.

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License and Dependencies

This module depends on:
- **NGINX core** (licensed under the 2-clause BSD license)
- **Red Hat Universal Base Image (UBI)** (subject to the Red Hat UBI End User License Agreement)

Please refer to the respective license texts for details:
- [NGINX License](https://www.nginx.com/resources/legal)
- [Red Hat UBI EULA](https://www.redhat.com/en/about/red-hat-end-user-license-agreements#UBI)

The code in this repository is licensed under the terms described in [LICENSE](LICENSE).