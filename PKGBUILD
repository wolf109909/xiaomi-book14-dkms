# Maintainer: reverse-engineered for Xiaomi Book Pro 14 2026
pkgname=xiaomi-book14-dkms
pkgver=1.2.0
pkgrel=4
pkgdesc="Platform driver for Xiaomi Book Pro 14 2026: suspend, lid, fan tach, power profiles (RAPL), settings key (DKMS)"
arch=('any')
license=('GPL-2.0-or-later')
depends=('dkms')
optdepends=('xiaomi-settings-key: userspace handler for the dedicated settings key')
install=xiaomi-book14-dkms.install
source=("xiaomi-book14.c"
        "Makefile"
        "dkms.conf"
        "xiaomi-book14-modprobe.conf"
        "xiaomi-book14-modules-load.conf"
        "99-xiaomi-book14-adp1-wakeup.rules"
        "xiaomi-book14-adp1-wakeup.service")
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')

package() {
    # DKMS source
    install -Dm644 xiaomi-book14.c "$pkgdir/usr/src/xiaomi-book14-$pkgver/xiaomi-book14.c"
    install -Dm644 Makefile        "$pkgdir/usr/src/xiaomi-book14-$pkgver/Makefile"
    install -Dm644 dkms.conf       "$pkgdir/usr/src/xiaomi-book14-$pkgver/dkms.conf"
    # blacklist broken mainline drivers, autoload this module
    install -Dm644 xiaomi-book14-modprobe.conf \
        "$pkgdir/usr/lib/modprobe.d/xiaomi-book14.conf"
    install -Dm644 xiaomi-book14-modules-load.conf \
        "$pkgdir/usr/lib/modules-load.d/xiaomi-book14.conf"
    install -Dm644 99-xiaomi-book14-adp1-wakeup.rules \
        "$pkgdir/usr/lib/udev/rules.d/99-xiaomi-book14-adp1-wakeup.rules"
    install -Dm644 xiaomi-book14-adp1-wakeup.service \
        "$pkgdir/usr/lib/systemd/system/xiaomi-book14-adp1-wakeup.service"
}
