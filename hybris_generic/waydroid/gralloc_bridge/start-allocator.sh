#!/system/bin/sh
# Run the host's Mali AIDL graphics allocator inside the Waydroid container:
# the container's own VINTF declares no AIDL allocator, and the A16 gralloc5
# path in libui needs one (it asks it for the mapper library suffix, then
# dlopens mapper.<suffix>.so — which the vendor image symlinks at the host's
# own /vendor_extra/lib64/hw/mapper.mediatek.so).  Its Mali deps resolve from
# /vendor_extra; the AIDL NDK libs it links against (allocator-V2-ndk,
# graphics.common-V4-ndk) are in the A16 image already.
export LD_LIBRARY_PATH=/vendor_extra/lib64/hw/mt6878:/vendor_extra/lib64:/vendor_extra/lib64/hw:/odm
ALLOC=$(ls /vendor_extra/bin/hw/*/android.hardware.graphics.allocator-V2-service-mediatek.* 2>/dev/null | head -1)
[ -z "$ALLOC" ] && ALLOC=/vendor_extra/bin/hw/android.hardware.graphics.allocator-V2-service-mediatek
exec "$ALLOC"
