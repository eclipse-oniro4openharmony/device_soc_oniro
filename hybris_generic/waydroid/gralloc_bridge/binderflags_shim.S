/* A14 AServiceManager_addServiceWithFlags(binder, instance, flags) forwarded
 * to A13 AServiceManager_addService(binder, instance) — drop the flags arg.
 * The import is WEAK so preloading the shim into wrappers (nohup/timeout/
 * init) that don't link libbinder_ndk does not fail; it resolves to the real
 * symbol only inside the allocator, which needs libbinder_ndk anyway. */
    .text
    .weak  AServiceManager_addService
    .globl AServiceManager_addServiceWithFlags
    .type  AServiceManager_addServiceWithFlags, %function
AServiceManager_addServiceWithFlags:
    b      AServiceManager_addService
    .size  AServiceManager_addServiceWithFlags, .-AServiceManager_addServiceWithFlags
