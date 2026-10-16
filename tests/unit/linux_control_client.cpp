#if defined(__linux__)

#include <gtest/gtest.h>

#include "virtual_display/driver/linux_control_client.h"

#include <chrono>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace virtual_display::driver {
  TEST(LinuxControlClient, RejectsUnknownConnectorBeforeTransport) {
    LinuxControlClient client {"/tmp/libvirtualdisplay-unused.sock", std::chrono::milliseconds {10}};
    for (const auto &name : {
           "Virtual-0", "Virtual-01", "Virtual-2147483648", "Virtual-8 extra", "Virtual-../8"
         }) {
      const auto result = client.query_connector(name);
      EXPECT_EQ(result.status, LinuxControlStatus::InvalidRequest) << name;
      EXPECT_FALSE(result.connected.has_value());
    }
  }

  TEST(LinuxControlClient, ReportsMissingBrokerAsTransportFailure) {
    LinuxControlClient client {"/tmp/libvirtualdisplay-missing.sock", std::chrono::milliseconds {10}};
    const auto result = client.query_connector("Virtual-1");
    EXPECT_EQ(result.status, LinuxControlStatus::TransportFailed);
    EXPECT_FALSE(result.connected.has_value());
  }

  TEST(LinuxControlClient, ParsesSocketActivatedBrokerReplyBeforeHangup) {
    const std::string socket_path = "/tmp/libvirtualdisplay-control-" + std::to_string(getpid()) + ".sock";
    unlink(socket_path.c_str());

    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    ASSERT_LT(socket_path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);

    std::string request;
    std::thread broker {[&]() {
      const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (connection < 0) {
        return;
      }
      char buffer[64];
      const auto count = recv(connection, buffer, sizeof(buffer), 0);
      if (count > 0) {
        request.assign(buffer, static_cast<std::size_t>(count));
        const std::string reply = "STATUS disconnected Virtual-9\n";
        (void) send(connection, reply.data(), reply.size(), MSG_NOSIGNAL);
      }
      close(connection);
    }};

    LinuxControlClient client {socket_path, std::chrono::seconds {1}};
    const auto result = client.query_connector("Virtual-9");
    broker.join();
    close(listener);
    unlink(socket_path.c_str());

    EXPECT_EQ(request, "status Virtual-9\n");
    ASSERT_TRUE(result.ok()) << result.detail;
    ASSERT_TRUE(result.connected.has_value());
    EXPECT_FALSE(*result.connected);
  }

  TEST(LinuxControlClient, AlwaysAuthenticatesIdempotentMutationWithBroker) {
    const std::string socket_path = "/tmp/libvirtualdisplay-owner-" + std::to_string(getpid()) + ".sock";
    unlink(socket_path.c_str());
    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    ASSERT_LT(socket_path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);

    std::string request;
    std::thread broker {[&]() {
      const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (connection < 0) {
        return;
      }
      char buffer[64];
      const auto count = recv(connection, buffer, sizeof(buffer), 0);
      if (count > 0) {
        request.assign(buffer, static_cast<std::size_t>(count));
        const std::string reply = "ERROR connector owned by another uid\n";
        (void) send(connection, reply.data(), reply.size(), MSG_NOSIGNAL);
      }
      close(connection);
    }};

    LinuxControlClient client {socket_path, std::chrono::seconds {1}};
    const auto result = client.set_connector("Virtual-6", true);
    broker.join();
    close(listener);
    unlink(socket_path.c_str());

    EXPECT_EQ(request, "connect Virtual-6\n");
    EXPECT_EQ(result.status, LinuxControlStatus::ProtocolError);
    EXPECT_EQ(result.detail, "ERROR connector owned by another uid");
  }
}  // namespace virtual_display::driver

#endif
