#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gbm.h>
#include <xf86drm.h>

namespace {
  using gbm_create_device_fn = gbm_device *(*)(int);
  using gbm_device_destroy_fn = void (*)(gbm_device *);
  using drm_get_devices2_fn = int (*)(std::uint32_t, drmDevicePtr[], int);
  using drm_get_device2_fn = int (*)(int, std::uint32_t, drmDevicePtr *);

  constexpr std::string_view kRenderPciEnvironment = "VIBESHINE_KWIN_RENDER_PCI";
  constexpr std::string_view kPreloadActiveEnvironment = "VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE";

  std::atomic<gbm_create_device_fn> real_gbm_create_device {nullptr};
  std::atomic<gbm_device_destroy_fn> real_gbm_device_destroy {nullptr};
  std::atomic<drm_get_devices2_fn> real_drm_get_devices2 {nullptr};
  std::atomic<drm_get_device2_fn> real_drm_get_device2 {nullptr};
  std::atomic<bool> gpu_attachment_logged {false};
  std::atomic<bool> ambiguous_gpu_logged {false};
  // The secure-mode loader trusts this library in any set-user-ID program.
  // Only the actual KWin compositor may route a virtual device's GBM allocator.
  std::atomic<bool> hooks_enabled {false};
  std::mutex render_fds_mutex;
  std::unordered_map<gbm_device *, int> owned_render_fds;

  template<typename Function>
  Function resolve_real_function(std::atomic<Function> &storage, const char *name) {
    auto function = storage.load(std::memory_order_acquire);
    if (function) {
      return function;
    }
    function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
    if (function) {
      storage.store(function, std::memory_order_release);
    }
    return function;
  }

  bool is_vibeshine_display(const drmDevicePtr device) {
    return device && device->bustype == DRM_BUS_FAUX && device->businfo.faux &&
      std::strcmp(device->businfo.faux->name, "vibeshine") == 0;
  }

  bool is_nvidia_render_device(const drmDevicePtr device) {
    return device && device->bustype == DRM_BUS_PCI && device->businfo.pci &&
      device->deviceinfo.pci && device->deviceinfo.pci->vendor_id == 0x10de &&
      (device->available_nodes & (1 << DRM_NODE_RENDER)) != 0 &&
      device->nodes && device->nodes[DRM_NODE_RENDER];
  }

  std::optional<drmPciBusInfo> parse_pci_device(const char *value) {
    unsigned int domain = 0;
    unsigned int bus = 0;
    unsigned int device = 0;
    unsigned int function = 0;
    char trailing = '\0';
    if (std::sscanf(value, "%x:%x:%x.%x%c", &domain, &bus, &device, &function, &trailing) != 4 ||
        domain > 0xffff || bus > 0xff || device > 0x1f || function > 7) {
      return std::nullopt;
    }
    return drmPciBusInfo {
      .domain = static_cast<std::uint16_t>(domain),
      .bus = static_cast<std::uint8_t>(bus),
      .dev = static_cast<std::uint8_t>(device),
      .func = static_cast<std::uint8_t>(function),
    };
  }

  bool pci_matches(const drmPciBusInfo &actual, const drmPciBusInfo &expected) {
    return actual.domain == expected.domain && actual.bus == expected.bus &&
      actual.dev == expected.dev && actual.func == expected.func;
  }

  drmDevicePtr select_render_device(drmDevicePtr devices[], const int count) {
    const auto *configured_value = std::getenv(kRenderPciEnvironment.data());
    const auto configured = configured_value && *configured_value ?
      parse_pci_device(configured_value) : std::optional<drmPciBusInfo> {};
    if (configured_value && *configured_value && !configured) {
      if (!ambiguous_gpu_logged.exchange(true, std::memory_order_relaxed)) {
        constexpr char message[] =
          "[vibeshine-kwin-gpu] invalid VIBESHINE_KWIN_RENDER_PCI; refusing virtual GPU association\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
      }
      return nullptr;
    }
    drmDevicePtr selected = nullptr;
    int matches = 0;
    for (int index = 0; index < count; ++index) {
      auto *const candidate = devices[index];
      if (!is_nvidia_render_device(candidate) ||
          (configured && !pci_matches(*candidate->businfo.pci, *configured))) {
        continue;
      }
      selected = candidate;
      ++matches;
    }
    if (matches == 1) {
      return selected;
    }
    if (matches > 1 && !ambiguous_gpu_logged.exchange(true, std::memory_order_relaxed)) {
      constexpr char message[] =
        "[vibeshine-kwin-gpu] multiple NVIDIA render devices found; set VIBESHINE_KWIN_RENDER_PCI for virtual outputs\n";
      (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    }
    return nullptr;
  }

  int open_render_device(drmPciBusInfo &identity) {
    const auto enumerate = resolve_real_function(real_drm_get_devices2, "drmGetDevices2");
    const auto get_device = resolve_real_function(real_drm_get_device2, "drmGetDevice2");
    if (!enumerate || !get_device) {
      return -1;
    }
    const int count = enumerate(0, nullptr, 0);
    if (count <= 0) {
      return -1;
    }
    std::vector<drmDevicePtr> devices(static_cast<std::size_t>(count), nullptr);
    const int actual_count = std::min(enumerate(0, devices.data(), count), count);
    if (actual_count <= 0) {
      return -1;
    }
    std::string render_node;
    if (const auto selected = select_render_device(devices.data(), actual_count)) {
      identity = *selected->businfo.pci;
      render_node = selected->nodes[DRM_NODE_RENDER];
    }
    drmFreeDevices(devices.data(), actual_count);
    if (render_node.empty()) {
      return -1;
    }
    const int fd = open(render_node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      return -1;
    }
    // A hotplug can renumber renderD* between enumeration and open. Bind only
    // the verified real render node for the selected PCI device.
    drmDevicePtr opened = nullptr;
    const bool matches = drmGetNodeTypeFromFd(fd) == DRM_NODE_RENDER &&
      get_device(fd, 0, &opened) == 0 && is_nvidia_render_device(opened) &&
      pci_matches(*opened->businfo.pci, identity);
    drmFreeDevice(&opened);
    if (!matches) {
      close(fd);
      return -1;
    }
    return fd;
  }

  void log_gpu_attachment(const drmPciBusInfo &identity) {
    if (gpu_attachment_logged.exchange(true, std::memory_order_relaxed)) {
      return;
    }
    char message[224] {};
    const auto length = std::snprintf(message, sizeof(message),
      "[vibeshine-kwin-gpu] virtual GBM uses NVIDIA render node %04x:%02x:%02x.%u; seat DRM devices unchanged\n",
      identity.domain, identity.bus, identity.dev, identity.func);
    if (length > 0) {
      (void)write(STDERR_FILENO, message,
        std::min(static_cast<std::size_t>(length), sizeof(message) - 1));
    }
  }

  bool running_inside_kwin() {
    std::array<char, 4096> executable {};
    const auto length = readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (length <= 0 || static_cast<std::size_t>(length) >= executable.size()) {
      return false;
    }
    executable[static_cast<std::size_t>(length)] = '\0';
    return std::strcmp(executable.data(), "/usr/bin/kwin_wayland") == 0;
  }

  __attribute__((constructor)) void stop_kwin_preload_inheritance() {
    if (!std::getenv(kPreloadActiveEnvironment.data()) || !running_inside_kwin()) {
      return;
    }
    hooks_enabled.store(true, std::memory_order_release);
    (void)unsetenv("LD_PRELOAD");
    (void)unsetenv(kPreloadActiveEnvironment.data());
  }
}

extern "C" gbm_device *gbm_create_device(const int fd) {
  const auto function = resolve_real_function(real_gbm_create_device, "gbm_create_device");
  if (!function) {
    errno = ENOSYS;
    return nullptr;
  }
  const int original_errno = errno;
  const auto pass_through = [&]() {
    errno = original_errno;
    return function(fd);
  };
  if (!hooks_enabled.load(std::memory_order_acquire)) {
    return pass_through();
  }
  const auto get_device = resolve_real_function(real_drm_get_device2, "drmGetDevice2");
  drmDevicePtr device = nullptr;
  const bool virtual_display = get_device && get_device(fd, 0, &device) == 0 &&
    is_vibeshine_display(device);
  drmFreeDevice(&device);
  if (!virtual_display) {
    return pass_through();
  }
  drmPciBusInfo identity {};
  const int render_fd = open_render_device(identity);
  if (render_fd < 0) {
    return pass_through();
  }
  // Route only the virtual node's allocator. KWin keeps the original KMS fd
  // and seat primary, while GBM/EGL see an actual render GPU and use their
  // normal per-output import path. Never spoof global libdrm PCI metadata.
  auto *const allocator = function(render_fd);
  if (!allocator) {
    close(render_fd);
    return pass_through();
  }
  {
    std::lock_guard lock {render_fds_mutex};
    owned_render_fds.emplace(allocator, render_fd);
  }
  log_gpu_attachment(identity);
  return allocator;
}

extern "C" void gbm_device_destroy(gbm_device *device) {
  const auto function = resolve_real_function(real_gbm_device_destroy, "gbm_device_destroy");
  if (!function) {
    errno = ENOSYS;
    return;
  }
  if (!hooks_enabled.load(std::memory_order_acquire)) {
    function(device);
    return;
  }
  int render_fd = -1;
  {
    std::lock_guard lock {render_fds_mutex};
    if (const auto found = owned_render_fds.find(device); found != owned_render_fds.end()) {
      render_fd = found->second;
      owned_render_fds.erase(found);
    }
  }
  function(device);
  // GBM does not own its input fd. Keep our render fd alive through destruction
  // and close only the fd opened by this bridge, never a caller's KMS fd.
  if (render_fd >= 0) {
    close(render_fd);
  }
}

extern "C" unsigned int vibeshine_kwin_gpu_interposer_abi() {
  return 5;
}
