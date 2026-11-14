// SPDX-License-Identifier: MIT
// Husky VK Metal plug-in: a Metal driver bundle for iOS 26.5 on the Pixel 8 Pro (Mali-G715), the metal2vulkan runtime over the Mesa panvk Vulkan
// driver. Ported from UniXNU's AdrenoMetal plug-in entry (MIT, NativeAdrenoMetalPlugin.mm), reduced to the discovery path:
//   Metal -> IOServiceMatching("IOAcceleratorES") -> property MetalPluginName ("HuskyVKMetal", published by the kernel with huskymetal=vk)
//   -> /System/Library/Extensions/HuskyVKMetal.bundle -> MetalPluginClassName / NSPrincipalClass (HuskyVKMetalDevice, a subclass of _MTLDevice)
//   -> [[cls alloc] initWithAcceleratorPort:] -> MTLAddDevice(returned object).
// The object this initializer returns is the frontend's device (HuskyVKMetalFrontend.mm), not the plug-in class's own instance, which only exists
// to satisfy the discovery's class check. Every process that loads the bundle gets its own device: its own panvk connection, GPU address space and
// shader cache.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <os/log.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <string>
#include "HuskyVKMetalFrontend.h"
#include "SharedShaderCache.h"
extern "C" {
#include "bridge/NativeMetalObjects.h"
}

@interface _MTLDevice : NSObject
@end
@interface HuskyVKMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

static os_log_t huskyLog() {
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("org.husky.vkmetal", "plugin"); });
    return log;
}

// The kernel console, which a bring-up run reads when a process's own log goes nowhere (as MaliMetal does).
static void huskyConsole(const char *format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (length <= 0) return;
    const int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        (void)!write(fd, text, (size_t)(length < (int)sizeof text ? length : (int)sizeof text - 1));
        close(fd);
    }
}

// Whether the kernel command line holds `key` or `key=...` as a whole token.
static const char *bootArgToken(const char *arguments, const char *prefix) {
    const size_t length = strlen(prefix);
    for (const char *at = arguments; (at = strstr(at, prefix)); at += length)
        if (at == arguments || at[-1] == ' ') return at;
    return nullptr;
}
static bool bootArgSet(const char *key) {
    char arguments[1100] = {};
    size_t size = sizeof arguments - 1;
    if (sysctlbyname("kern.bootargs", arguments, &size, nullptr, 0) != 0) return false;
    const char *at = bootArgToken(arguments, key);
    const size_t length = strlen(key);
    return at && (at[length] == 0 || at[length] == ' ');
}

// QuartzCore reads its boolean options once; set the environment variable and, if QuartzCore is loaded, the live option (CASetDebugOption).
// With a device that answers isFramebufferReadSupported NO, CoreAnimation builds blended "generic" pipelines with blending off while a specialised
// one compiles; CA_DISABLE_GENERIC_SHADERS makes it wait instead. Same as MaliMetal (MaliMTLDevice.m).
static void setCoreAnimationOption(const char *option, const char *opposite) {
    if (getenv(option) || (opposite && getenv(opposite))) return;
    setenv(option, "1", 0);
    const char *(*optionName)(int) = (const char *(*)(int))dlsym(RTLD_DEFAULT, "CAGetDebugOptionEnvVariableName");
    void (*setOption)(int, bool) = (void (*)(int, bool))dlsym(RTLD_DEFAULT, "CASetDebugOption");
    if (!optionName || !setOption) return;
    for (int i = 0; i < 4096; i++) {
        const char *name = optionName(i);
        if (!name) break;
        if (strcmp(name, option)) continue;
        // Device creation can run inside CoreAnimation's own calls: set the live option from a global queue.
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ setOption(i, true); });
        return;
    }
}

// The Vulkan driver the runtime opens as its loader: it exports vk_icdGetInstanceProcAddr (the runtime takes that when the library has no
// vkGetInstanceProcAddr), so there is no Khronos loader. HUSKYVK_ICD overrides; then the bundle's Frameworks directory; then the system path.
static bool vulkanDriverPath(char *path, size_t size) {
    if (const char *override = getenv("HUSKYVK_ICD")) {
        strlcpy(path, override, size);
        return access(path, R_OK) == 0;
    }
    Dl_info info = {};
    if (dladdr(reinterpret_cast<const void *>(&vulkanDriverPath), &info) && info.dli_fname) {
        strlcpy(path, info.dli_fname, size);
        if (char *slash = strrchr(path, '/')) {
            *slash = 0;
            strlcat(path, "/Frameworks/libvulkan_panfrost.dylib", size);
            if (access(path, R_OK) == 0) return true;
        }
    }
    strlcpy(path, "/usr/local/lib/libvulkan_panfrost.dylib", size);
    return access(path, R_OK) == 0;
}

static id deviceForPort(unsigned port) {
    static id device;
    static bool tried;
    static NSObject *lock = [NSObject new];
    @synchronized(lock) {
        if (tried) return device;
        tried = true;
        // Boot-argument switches, each mirrored into the environment the frontend reads.
        if (bootArgSet("huskyvk-codegen=1")) setenv("HUSKYVK_CODEGEN", "1", 1);
        if (bootArgSet("diag=1")) setenv("HUSKYVK_DIAG", "1", 1);
        if (bootArgSet("log-gpu=1")) setenv("HUSKYVK_LOG_GPU", "1", 1);
        if (bootArgSet("huskyvk-nodmabuf=1")) setenv("HUSKYVK_NO_SURFACE_DMABUF", "1", 1);
        if (bootArgSet("gpu-purge=0")) setenv("NATIVE_METAL_PURGE", "0", 1);
        // A pipeline making that compiles inside the call stays on the asking thread (runtime patch 0041).
        setenv("METAL2VULKAN_COMPILE_ON_ASKING_THREAD", "1", 1);
        // Planes the GPU writes must be plain rows: Metal's IOSurface compression layouts are AGX's and unknown to Mali.
        setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
        CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);

        char driver[1024];
        if (!vulkanDriverPath(driver, sizeof driver)) {
            huskyConsole("HuskyVKMetal: no Vulkan driver (libvulkan_panfrost.dylib) for %s\n", getprogname());
            os_log_error(huskyLog(), "no libvulkan_panfrost.dylib (set HUSKYVK_ICD or install /usr/local/lib/libvulkan_panfrost.dylib)");
            return nil;
        }
        uint64_t registryID = 0;
        if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
            // The discovery releases the port when this returns; the device keeps its own reference.
            if (auto retain = (int (*)(unsigned))dlsym(iokit, "IOObjectRetain")) retain(port);
            if (auto entryID = (int (*)(unsigned, uint64_t *))dlsym(iokit, "IORegistryEntryGetRegistryEntryID")) entryID(port, &registryID);
        }
        native_bridge_metal_set_accelerator_port(port);

        // The runtime's shader cache: the same-user shared directory, then Metal's own cache directory, then the temporary one; the first this
        // process's sandbox lets it write. No directory at all runs the runtime without a cache.
        char candidates[3][512] = {};
        strlcpy(candidates[0], sharedShaderCacheDirectory().c_str(), sizeof candidates[0]);
        if (!confstr(_CS_DARWIN_USER_CACHE_DIR, candidates[1], sizeof candidates[1] - 32)) candidates[1][0] = 0;
        if (!confstr(_CS_DARWIN_USER_TEMP_DIR, candidates[2], sizeof candidates[2] - 32)) candidates[2][0] = 0;
        const char *lastError = "";
        unsigned usable = 0;
        for (unsigned i = 0; i < 3 && !device; i++) {
            if (!candidates[i][0]) continue;
            if (i == 1) {
                strlcat(candidates[i], "com.apple.metal", sizeof candidates[i]);
                mkdir(candidates[i], 0700);
                strlcat(candidates[i], "/", sizeof candidates[i]);
            }
            if (i != 0) strlcat(candidates[i], "m2v", sizeof candidates[i]);
            mkdir(candidates[i], 0700);
            if (access(candidates[i], W_OK) != 0) continue;
            ++usable;
            @try {
                device = native_bridge_metal_open(driver, candidates[i], registryID, nullptr);
            } @catch (NSException *exception) {
                device = nil;
                lastError = "exception";
            }
            if (!device && !lastError[0]) lastError = nmb_last_error();
        }
        if (!device && !usable) {
            setenv("METAL2VULKAN_SHADER_CACHE_OPTIONAL", "1", 1);
            const char *path = candidates[1][0] ? candidates[1] : candidates[2][0] ? candidates[2] : "/var/mobile/tmp/m2v";
            @try {
                device = native_bridge_metal_open(driver, path, registryID, nullptr);
            } @catch (NSException *exception) {
                device = nil;
                lastError = "exception";
            }
            if (!device && !lastError[0]) lastError = nmb_last_error();
        }
        if (!device) {
            huskyConsole("HuskyVKMetal: no device in %s (%s)\n", getprogname(), lastError);
            os_log_error(huskyLog(), "no device: %{public}s", lastError);
            return nil;
        }
        if (![(id<MTLDevice>)device respondsToSelector:@selector(isFramebufferReadSupported)] ||
            !((BOOL (*)(id, SEL))objc_msgSend)(device, sel_registerName("isFramebufferReadSupported")))
            setCoreAnimationOption("CA_DISABLE_GENERIC_SHADERS", "CA_FORCE_GENERIC_SHADERS");
        huskyConsole("HuskyVKMetal: device up in %s (driver %s)\n", getprogname(), driver);
        os_log(huskyLog(), "device up in %{public}s, driver %{public}s", getprogname(), driver);
        return device;
    }
}

@implementation HuskyVKMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self = [super init];
    if (!self || !port) return nil;
    return deviceForPort(port);
}
@end
