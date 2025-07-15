# Makefile for building ngx_steadybit_sleep_block_module as a dynamic module in build/ directory

BUILD_DIR = build
DIST_DIR = dist
NGINX_VERSION ?= 1.27.4
NGINX_TARBALL = $(BUILD_DIR)/nginx-$(NGINX_VERSION).tar.gz
NGINX_SRC_DIR = $(BUILD_DIR)/nginx-$(NGINX_VERSION)
MODULE_DIR = ngx_steadybit_sleep_block_module
MODULE_SRC = $(MODULE_DIR)/ngx_steadybit_sleep_block_module.c
MODULE_CONFIG = $(MODULE_DIR)/config
MODULE_NAME = ngx_steadybit_sleep_block_module
MODULE_SO = $(NGINX_SRC_DIR)/objs/$(MODULE_NAME).so
DIST_SO = $(DIST_DIR)/$(MODULE_NAME).so

.PHONY: all clean distclean

all: $(DIST_SO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

$(NGINX_TARBALL): | $(BUILD_DIR)
	wget -O $(NGINX_TARBALL) http://nginx.org/download/nginx-$(NGINX_VERSION).tar.gz

$(NGINX_SRC_DIR): $(NGINX_TARBALL)
	tar -xzf $(NGINX_TARBALL) -C $(BUILD_DIR)

$(NGINX_SRC_DIR)/src/http/modules/ngx_steadybit_sleep_block_module.c: $(MODULE_SRC) | $(NGINX_SRC_DIR)/src/http/modules
	cp $(MODULE_SRC) $(NGINX_SRC_DIR)/src/http/modules/

$(NGINX_SRC_DIR)/src/http/modules/config: $(MODULE_CONFIG) | $(NGINX_SRC_DIR)/src/http/modules
	cp $(MODULE_CONFIG) $(NGINX_SRC_DIR)/src/http/modules/config

$(NGINX_SRC_DIR)/src/http/modules:
	mkdir -p $(NGINX_SRC_DIR)/src/http/modules

$(MODULE_SO): $(NGINX_SRC_DIR) $(NGINX_SRC_DIR)/src/http/modules/ngx_steadybit_sleep_block_module.c $(NGINX_SRC_DIR)/src/http/modules/config
	cd $(NGINX_SRC_DIR) && ./configure --with-compat --add-dynamic-module=src/http/modules
	$(MAKE) -C $(NGINX_SRC_DIR) modules

$(DIST_SO): $(MODULE_SO) | $(DIST_DIR)
	cp $(MODULE_SO) $(DIST_SO)

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)

distclean: clean
