#!/bin/sh
# ZCI + ZEKANS Ortak Gemma 4 E4B Motoru ve Çift Gateway Başlatıcısı
# Portlar: 29087 (Ortak Engine), 19086 (ZCI Sohbet Gateway), 29016 (ZEKANS Kodlama Gateway)
# ModelLease (/run/user/<uid>/zethropol-inference/cpu.lock), --parallel 1 ve 12 GiB RAM koruması ile çalışır.
# Mevcut 8087/8088 servislerini, start-zci-gemma4.sh ve zci-code-4b başlatıcılarını değiştirmez.

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec /usr/bin/python3 -I -B "$HERE/shared_gemma.py" serve "$@"
