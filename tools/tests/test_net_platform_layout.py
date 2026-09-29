"""验证 LikesProgramNet 平台实现集中在统一私有目录。"""

from pathlib import Path
import unittest


REPOSITORY_ROOT = Path(__file__).parents[2]
NET_ROOT = REPOSITORY_ROOT / "packages" / "LikesProgramNet"

PLATFORM_SOURCES = {
    "posix": (
        "ReadinessCompletionPoller.cpp",
    ),
    "linux": (
        "EpollDriver.cpp",
        "EventLoopWakeup.cpp",
        "IoUringPoller.cpp",
        "LinuxPollerBackendPolicy.cpp",
        "LinuxPollerFactory.cpp",
        "SocketOps.cpp",
        "UdpBatchOps.cpp",
    ),
    "windows": (
        "EventLoopWakeup.cpp",
        "IocpPoller.cpp",
        "SocketOps.cpp",
        "UdpBatchOps.cpp",
        "WindowsPollerBackendPolicy.cpp",
        "WindowsPollerFactory.cpp",
    ),
}

PLATFORM_HEADERS = {
    "posix": (
        "ReadinessCompletionPoller.hpp",
    ),
    "linux": (
        "EpollDriver.hpp",
        "IoUringPoller.hpp",
        "LinuxPollerBackendPolicy.hpp",
    ),
    "windows": (
        "IocpOperation.hpp",
        "IocpPoller.hpp",
        "WindowsPollerBackendPolicy.hpp",
    ),
}

COMMON_PLATFORM_SOURCES = ()
COMMON_PLATFORM_HEADERS = (
    "EventLoopWakeup.hpp",
    "ReadinessDriver.hpp",
    "SocketOps.hpp",
    "UdpBatchOps.hpp",
)


class NetPlatformLayoutTests(unittest.TestCase):
    def test_platform_sources_live_under_src_platform(self) -> None:
        platform_root = NET_ROOT / "src" / "platform"
        for platform, filenames in PLATFORM_SOURCES.items():
            for filename in filenames:
                with self.subTest(platform=platform, filename=filename):
                    self.assertTrue((platform_root / platform / filename).is_file())
                    self.assertFalse((platform_root / filename).exists())
                    self.assertFalse((NET_ROOT / "src" / filename).exists())
        for filename in COMMON_PLATFORM_SOURCES:
            with self.subTest(platform="common", filename=filename):
                self.assertTrue((platform_root / filename).is_file())
                self.assertFalse((NET_ROOT / "src" / filename).exists())

    def test_platform_headers_live_under_private_platform_directory(self) -> None:
        platform_root = NET_ROOT / "src" / "include" / "net" / "platform"
        for platform, filenames in PLATFORM_HEADERS.items():
            for filename in filenames:
                with self.subTest(platform=platform, filename=filename):
                    self.assertTrue((platform_root / platform / filename).is_file())
                    self.assertFalse((platform_root / filename).exists())
                    self.assertFalse((NET_ROOT / "src" / "include" / "net" / filename).exists())
        for filename in COMMON_PLATFORM_HEADERS:
            with self.subTest(platform="common", filename=filename):
                self.assertTrue((platform_root / filename).is_file())
                self.assertFalse((NET_ROOT / "src" / "include" / "net" / filename).exists())

    def test_generic_transports_do_not_embed_platform_io(self) -> None:
        forbidden_fragments = (
            "#ifdef _WIN32",
            "#if defined(__linux__)",
            "::recv(",
            "::send(",
            "::recvfrom(",
            "::sendto(",
            "::recvmmsg(",
            "::sendmmsg(",
        )
        for filename in ("TcpTransport.cpp", "UdpTransport.cpp"):
            source = (NET_ROOT / "src" / filename).read_text(encoding="utf-8")
            for fragment in forbidden_fragments:
                with self.subTest(filename=filename, fragment=fragment):
                    self.assertNotIn(fragment, source)

    def test_generic_net_sources_use_socket_ops(self) -> None:
        forbidden_fragments = ("::bind(", "::listen(", "::connect(", "::accept(", "::accept4(")
        for filename in ("Client.cpp", "Server.cpp"):
            source = (NET_ROOT / "src" / filename).read_text(encoding="utf-8")
            for fragment in forbidden_fragments:
                with self.subTest(filename=filename, fragment=fragment):
                    self.assertNotIn(fragment, source)

    def test_current_tree_contains_only_private_platform_sources(self) -> None:
        platform_root = NET_ROOT / "src" / "platform"
        private_root = NET_ROOT / "src" / "include" / "net" / "platform"
        direct_platform_sources = (
            "EpollDriver.cpp",
            "EventLoopWakeup.cpp",
            "IoUringPoller.cpp",
            "LinuxPollerFactory.cpp",
            "ReadinessCompletionPoller.cpp",
            "SocketOps.cpp",
            "UdpBatchOps.cpp",
            "EventLoopWakeup.cpp",
            "IocpPoller.cpp",
            "WindowsPollerBackendPolicy.cpp",
            "WindowsPollerFactory.cpp",
        )
        direct_platform_headers = (
            "EpollDriver.hpp",
            "IoUringPoller.hpp",
            "LinuxPollerBackendPolicy.hpp",
            "ReadinessCompletionPoller.hpp",
            "ReadinessDriver.hpp",
            "SocketOps.hpp",
            "UdpBatchOps.hpp",
            "IocpOperation.hpp",
            "IocpPoller.hpp",
            "WindowsPollerBackendPolicy.hpp",
        )
        public_root = NET_ROOT / "include" / "LikesProgram" / "Net"
        for filename in direct_platform_sources:
            with self.subTest(direct_source=filename):
                self.assertFalse((NET_ROOT / "src" / filename).exists())
        for filename in direct_platform_headers:
            with self.subTest(direct_header=filename):
                self.assertFalse((public_root / filename).exists())
        self.assertFalse((platform_root / "bsd").exists())
        self.assertFalse((private_root / "bsd").exists())
        self.assertFalse((platform_root / "PollerBackend.cpp").exists())
        self.assertFalse((platform_root / "linux" / "EpollPoller.cpp").exists())
        self.assertFalse((NET_ROOT / "src" / "LinuxPollerFactory.cpp").exists())
        self.assertFalse((platform_root / "linux" / "CompletionEngine.cpp").exists())
        self.assertFalse((private_root / "CompletionEngine.hpp").exists())
        self.assertFalse((private_root / "PollerBackend.hpp").exists())
        for filename in (
            "EpollDriver.hpp",
            "IoUringPoller.hpp",
            "LinuxPollerBackend.hpp",
            "LinuxPollerBackendPolicy.hpp",
            "LinuxPollerFactory.hpp",
            "PollerBackend.hpp",
        ):
            with self.subTest(public_header=filename):
                self.assertFalse((public_root / filename).exists())
        self.assertFalse((private_root / "linux" / "EpollPoller.hpp").exists())

    def test_io_uring_is_required_not_optional(self) -> None:
        cmake = (NET_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn("LIKESPROGRAM_NET_ENABLE_IO_URING", cmake)
        self.assertNotIn("LP_NET_HAS_IO_URING", cmake)

    def test_windows_platform_boundary_has_no_readiness_or_linux_dependency(self) -> None:
        windows_root = NET_ROOT / "src" / "platform" / "windows"
        for path in windows_root.glob("*.cpp"):
            source = path.read_text(encoding="utf-8")
            for fragment in ("select(", "WSAPoll", "liburing", "EpollDriver"):
                with self.subTest(path=path.name, fragment=fragment):
                    self.assertNotIn(fragment, source)

    def test_plain_tcp_main_path_does_not_construct_public_transport(self) -> None:
        for filename in ("Connection.cpp", "Client.cpp", "Server.cpp"):
            source = (NET_ROOT / "src" / filename).read_text(encoding="utf-8")
            with self.subTest(filename=filename):
                self.assertNotIn("make_unique<TcpTransport>", source)

    def test_net_aggregate_does_not_advertise_legacy_transport(self) -> None:
        aggregate = (NET_ROOT / "include" / "LikesProgram" / "Net" / "Net.hpp").read_text(encoding="utf-8")
        for header in ("Transport.hpp", "TcpTransport.hpp", "UdpTransport.hpp"):
            with self.subTest(header=header):
                self.assertNotIn(header, aggregate)


if __name__ == "__main__":
    unittest.main()
