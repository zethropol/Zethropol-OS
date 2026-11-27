#!/bin/sh
# ZCI Gemma 4 E4B Bağımsız Sohbet Başlatıcısı
# Portlar: 19086 (Gateway), 19087 (Engine)
# ModelLease (/run/user/<uid>/cpu.lock) ve 12 GiB RAM koruması ile çalışır.
# Mevcut 8087/8088 hizmetlerini veya varsayılan modeli değiştirmez.

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec /usr/bin/python3 -I -B "$HERE/zci_chat.py" --config "$HERE/config.gemma4.example.json" "$@"
