#!/system/bin/sh
# W3 gate 2: run the host A14 Mali AIDL graphics allocator inside the A13
# container. The one A14-only libbinder_ndk symbol is trampolined by
# libbinderflags_shim.so; its Mali deps resolve from /vendor_extra.
export LD_PRELOAD=/odm/libbinderflags_shim.so
export LD_LIBRARY_PATH=/vendor_extra/lib64/hw/mt6878:/vendor_extra/lib64:/vendor_extra/lib64/hw:/odm
ALLOC=$(ls /vendor_extra/bin/hw/*/android.hardware.graphics.allocator-V2-service-mediatek.* 2>/dev/null | head -1)
[ -z "$ALLOC" ] && ALLOC=/vendor_extra/bin/hw/android.hardware.graphics.allocator-V2-service-mediatek
exec "$ALLOC"
