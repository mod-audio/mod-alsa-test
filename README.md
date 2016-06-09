mod-alsa-test
=============

A small tool to test alsa-i/o, aimed to aid alsa-driver development.

Buildroot package install
-------------------------

mod-alsa-test/Config.in

```
config BR2_PACKAGE_MOD_ALSA_TEST
	bool "mod-alsa-test"
	depends on BR2_USE_MMU
	depends on BR2_TOOLCHAIN_HAS_THREADS
	select BR2_PACKAGE_ALSA_LIB
	help
	  alsa audio test tool, written for moddevices.com
```


mod-alsa-test/mod-alsa-test.mk  (update git revision)
```
MOD_ALSA_TEST_VERSION = 94e9a90255d29dc741c584ffd6883f25b3e27362
MOD_ALSA_TEST_SITE = git://github.com/moddevices/mod-alsa-test.git
MOD_ALSA_TEST_LICENSE = GPLv2+
MOD_ALSA_TEST_DEPENDENCIES = host-pkgconf alsa-lib

define MOD_ALSA_TEST_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D) all
endef

define MOD_ALSA_TEST_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) DESTDIR="$(TARGET_DIR)" -C $(@D) install
endef

$(eval $(generic-package))
```
