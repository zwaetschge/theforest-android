#!/bin/sh
# Build the open-source Mono 4.8 (unity-5.6-mbe) BCL assemblies required by The Forest
# and copy them into build/mono-bcl/ and android/app/src/main/assets/mono_bcl/.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/third_party/mono
DST=$ROOT/build/mono-bcl
ASSETS=$ROOT/android/app/src/main/assets/mono_bcl

if [ ! -f "$SRC/configure" ]; then
  (cd "$SRC" && NOCONFIGURE=1 ./autogen.sh)
fi

mkdir -p "$DST" "$ASSETS"

ASSEMBLIES="corlib System System.XML Mono.Security System.Configuration System.Core System.Security System.Transactions Mono.Data.Tds System.EnterpriseServices System.Numerics System.Xml.Linq Mono.Posix"
DLLS="mscorlib.dll System.dll System.Xml.dll Mono.Security.dll System.Configuration.dll System.Core.dll System.Security.dll System.Transactions.dll Mono.Data.Tds.dll System.EnterpriseServices.dll System.Numerics.dll System.Xml.Linq.dll Mono.Posix.dll"

for dir in $ASSEMBLIES; do
  make -C "$SRC/mcs/class/$dir" PROFILE=net_4_x RUNTIME=mono MCS=mcs RESGEN=true CIL_STRINGREPLACER=true
done

for dll in $DLLS; do
  cp "$SRC/mcs/class/lib/net_4_x/$dll" "$DST/$dll"
  cp "$SRC/mcs/class/lib/net_4_x/$dll" "$ASSETS/$dll"
done

ls -lh "$ASSETS"
