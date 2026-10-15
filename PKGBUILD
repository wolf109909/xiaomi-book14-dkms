# Maintainer: Yuming Sun <wolf109909@outlook.com>
pkgname=xiaomi-book14-dkms
_modname=xiaomi-book14
pkgver=1.3.0
pkgrel=1
pkgdesc="Platform driver for Xiaomi Book Pro 14 2026: suspend, lid, fan tach, power profiles (RAPL), settings key (DKMS)"
arch=('any')
url="https://github.com/wolf109909/xiaomi-book14-dkms"
license=('GPL-2.0-or-later')
depends=('dkms')
optdepends=('xiaomi-settings-key: userspace handler for the dedicated settings key')
install=xiaomi-book14-dkms.install
source=("xiaomi-book14.c"
        "Makefile"
        "dkms.conf"
        "LICENSE"
        "xiaomi-book14-modprobe.conf"
        "xiaomi-book14-modules-load.conf"
        "99-xiaomi-book14-adp1-wakeup.rules"
        "xiaomi-book14-adp1-wakeup.service")
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')

package() {
    local _src="$pkgdir/usr/src/$_modname-$pkgver"

    # DKMS source (the dkms pacman hooks build it for every installed kernel)
    install -Dm644 xiaomi-book14.c Makefile -t "$_src"
    install -Dm644 dkms.conf "$_src/dkms.conf"
    sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$pkgver\"/" "$_src/dkms.conf"

    # blacklist broken mainline drivers, autoload this module
    install -Dm644 xiaomi-book14-modprobe.conf \
        "$pkgdir/usr/lib/modprobe.d/xiaomi-book14.conf"
    install -Dm644 xiaomi-book14-modules-load.conf \
        "$pkgdir/usr/lib/modules-load.d/xiaomi-book14.conf"

    # closed-lid AC/USB-C wake workaround
    install -Dm644 99-xiaomi-book14-adp1-wakeup.rules \
        "$pkgdir/usr/lib/udev/rules.d/99-xiaomi-book14-adp1-wakeup.rules"
    install -Dm644 xiaomi-book14-adp1-wakeup.service \
        "$pkgdir/usr/lib/systemd/system/xiaomi-book14-adp1-wakeup.service"

    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
