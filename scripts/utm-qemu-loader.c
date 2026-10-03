/* Development launcher for the QEMU libraries shipped with UTM. The guest
 * uses a VirtIO PCI GPU, exactly as it does with a standalone QEMU binary. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *path = getenv("ARK_UTM_QEMU_LIBRARY");
    if (!path) path = "/Applications/UTM.app/Contents/Frameworks/qemu-x86_64-softmmu.framework/Versions/A/qemu-x86_64-softmmu";
    void *library = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    void (*initialize)(int, char **, char **) = dlsym(library, "qemu_init");
    void (*run)(void) = dlsym(library, "qemu_main_loop");
    int (*cleanup)(void) = dlsym(library, "qemu_cleanup");
    if (!initialize || !run || !cleanup) { fprintf(stderr, "QEMU entry points unavailable\n"); return 1; }
    extern char **environ;
    initialize(argc, argv, environ);
    if (getenv("ARK_GPU_LOG")) {
        void *gles = dlopen("/Applications/UTM.app/Contents/Frameworks/GLESv2.framework/Versions/A/GLESv2", RTLD_NOW | RTLD_GLOBAL);
        const unsigned char *(*string)(unsigned) = gles ? dlsym(gles, "glGetString") : NULL;
        const unsigned char *renderer = string ? string(0x1f01) : NULL;
        if (renderer) fprintf(stderr, "[qemu-gpu] GL renderer: %s\n", renderer);
    }
    run();
    return cleanup();
}
