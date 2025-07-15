# nginx_steadybit_module

## Overview

`ngx_steadybit_sleep_block_module` is a third-party NGINX module that allows you to introduce artificial delays (sleep) into HTTP request processing. This is useful for testing, chaos engineering, and simulating network latency in development or staging environments.

## Features
- Add a configurable delay to HTTP requests using a custom directive
- Supports millisecond granularity
- Designed for use with NGINX and NGINX Ingress (based on UBI-compatible images)

## Requirements
- NGINX (built from source, or compatible with dynamic modules)
- Compatible with NGINX Ingress images based on UBI-compatible images (e.g., `nginx/nginx-ingress:5.0.0-ubi`). This is not an official Red Hat image.
- GCC, make, and standard build tools (for building the module)

## Build Instructions

### Using Makefile (local build)
```sh
make
# The built module will be in dist/ngx_steadybit_sleep_block_module.so
```

### Using Docker (recommended for compatibility)

Two example Dockerfiles are provided to demonstrate how to compile and use this module:

#### Dockerfile.community
Builds the module using the open-source NGINX base image. This is suitable for:
- Development and testing environments
- Open-source NGINX deployments
- Community-based NGINX Ingress Controller

```sh
docker build -f Dockerfile.community -t steadybit/nginx-sleep-module:community .
```

#### Dockerfile.ubi
Builds the module using Red Hat Universal Base Image (UBI) with NGINX. This is suitable for:
- Enterprise environments
- Red Hat-based deployments
- OpenShift deployments
- NGINX Ingress Controller based on UBI-compatible images (e.g., `nginx/nginx-ingress:5.0.0-ubi`). This is not an official Red Hat image.

```sh
docker build -f Dockerfile.ubi -t steadybit/nginx-sleep-module:ubi .
```

**Note**: These Dockerfiles serve as examples showing how to compile and integrate the module into different NGINX base images. You can adapt them to your specific NGINX Ingress Controller image and requirements.

## Usage

1. **Copy the built `.so` file** to your NGINX modules directory (e.g., `/usr/lib/nginx/modules/`).
2. **Load the module** in your `nginx.conf`:
   ```nginx
   load_module modules/ngx_steadybit_sleep_block_module.so;
   ```
3. **Use the directive** in your server/location block:
   ```nginx
   location /test-sleep {
       sb_sleep_ms 500;  # Sleep for 500 milliseconds
       proxy_pass http://backend;
   }
   ```

## NGINX Ingress Controller Usage

When using this module with NGINX Ingress Controller, additional configuration is required:

### 1. Enable Snippets
Snippets must be enabled on your NGINX Ingress Controller:
```bash
# Method 1: Controller arguments
--enable-snippets

# Method 2: ConfigMap option
kubectl patch configmap <ingress-configmap> -n <namespace> --type=merge -p '{"data":{"enable-snippets":"true"}}'

# Method 3: NGINX Ingress Operator
kubectl patch NginxIngress nginxingress-controller -n nginx-ingress --type=merge -p '{"spec":{"controller":{"enableSnippets":true}}}'
```

### 2. Load Module via ConfigMap
Load the module using the ConfigMap approach:
```bash
kubectl patch configmap nginxingress-controller-nginx-ingress \
  -n nginx-ingress \
  --type=merge \
  -p '{
    "data": {
      "main-snippets": "load_module /usr/lib/nginx/modules/ngx_steadybit_sleep_block_module.so;"
    }
  }'
```

### 3. Use in Ingress Annotations
The module can then be used via configuration snippets in Ingress resources:
```yaml
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: example-ingress
  annotations:
    nginx.ingress.kubernetes.io/configuration-snippet: |
      sb_sleep_ms 500;
spec:
  # ... ingress spec
```

## Configuration Example
```nginx
load_module modules/ngx_steadybit_sleep_block_module.so;

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
cd ngx_steadybit_sleep_block_module
bash test-sleep-module.sh
```
This will build NGINX with the module, start a test server, and run latency tests against the endpoints.

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License and Dependencies

This module depends on:
- **NGINX core** (licensed under the 2-clause BSD license)
- **Universal Base Image (UBI-compatible)** (subject to the Red Hat UBI End User License Agreement; this image is not provided or endorsed by Red Hat)

Please refer to the respective license texts for details:
- [NGINX License](https://www.nginx.com/resources/legal)
- [Red Hat UBI EULA](https://www.redhat.com/en/about/red-hat-end-user-license-agreements#UBI)

The code in this repository is licensed under the terms described in [LICENSE](LICENSE).

## Disclaimer

This project and its container images are not affiliated with, endorsed by, or supported by Red Hat. The images are based on UBI-compatible images but are not official Red Hat images. Any references to UBI are for compatibility purposes only.