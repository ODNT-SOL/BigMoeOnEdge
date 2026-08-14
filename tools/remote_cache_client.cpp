// Standalone TCP client for the F3 remote expert-cache prototype (Phase 3a).
//
// Fetches raw byte ranges from a remote_cache_server and reports throughput.
//
// Usage:
//   remote_cache_client <server_addr:port> <file> <offset> <size> [iterations]
//
// Protocol:
//   request  : [u64 offset][u64 size]  (16 bytes)
//   response : [u64 size][bytes...]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

static bool parse_addr(const char * s, std::string & host, int & port) {
    const char * colon = std::strrchr(s, ':');
    if (!colon || colon == s) return false;
    host.assign(s, colon - s);
    port = std::atoi(colon + 1);
    return port > 0 && port <= 65535;
}

static int connect_socket(const char * host, int port) {
    struct addrinfo hints{}, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_s[16];
    std::snprintf(port_s, sizeof(port_s), "%d", port);
    if (getaddrinfo(host, port_s, &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto * p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static bool recv_fully(int fd, void * buf, size_t n) {
    char * p = static_cast<char *>(buf);
    while (n) {
        ssize_t r = recv(fd, p, n, MSG_WAITALL);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

static bool send_fully(int fd, const void * buf, size_t n) {
    const char * p = static_cast<const char *>(buf);
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

static void encode_u64(uint8_t * p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t((v >> (i * 8)) & 0xff);
}

static uint64_t decode_u64(const uint8_t * p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (i * 8);
    return v;
}

int main(int argc, char ** argv) {
    if (argc < 5 || argc > 6) {
        std::fprintf(stderr, "Usage: %s <server_addr:port> <file> <offset> <size> [iterations]\n", argv[0]);
        return 1;
    }
    std::string host;
    int port = 0;
    if (!parse_addr(argv[1], host, port)) {
        std::fprintf(stderr, "Bad server address: %s\n", argv[1]);
        return 1;
    }
    const char * path = argv[2];
    uint64_t offset = std::strtoull(argv[3], nullptr, 10);
    uint64_t size   = std::strtoull(argv[4], nullptr, 10);
    int iterations = (argc >= 6) ? std::atoi(argv[5]) : 1;

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        std::perror("open local file");
        return 1;
    }

    int fd = connect_socket(host.c_str(), port);
    if (fd < 0) {
        std::perror("connect");
        return 1;
    }

    std::vector<uint8_t> local_buf(size_t(size), 0);
    ssize_t local_n = pread(file_fd, local_buf.data(), size_t(size), off_t(offset));
    close(file_fd);
    if (local_n != ssize_t(size)) {
        std::fprintf(stderr, "Could not read %lu bytes locally\n", size);
        return 1;
    }

    std::vector<uint8_t> remote_buf(size_t(size), 0);
    uint8_t req[16];
    encode_u64(req, offset);
    encode_u64(req + 8, size);

    auto t0 = std::chrono::steady_clock::now();
    uint64_t total_bytes = 0;
    for (int i = 0; i < iterations; ++i) {
        if (!send_fully(fd, req, 16)) {
            std::perror("send");
            return 1;
        }
        uint8_t hdr[8];
        if (!recv_fully(fd, hdr, 8)) {
            std::perror("recv header");
            return 1;
        }
        uint64_t resp_size = decode_u64(hdr);
        if (resp_size != size) {
            std::fprintf(stderr, "Unexpected response size: %lu != %lu\n", resp_size, size);
            return 1;
        }
        if (!recv_fully(fd, remote_buf.data(), size_t(resp_size))) {
            std::perror("recv payload");
            return 1;
        }
        total_bytes += resp_size;
        if (std::memcmp(local_buf.data(), remote_buf.data(), size_t(size)) != 0) {
            std::fprintf(stderr, "Byte mismatch on iteration %d\n", i + 1);
            return 1;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("OK: %d x %lu bytes in %.3f s = %.2f MiB/s\n",
                iterations, size, secs, (total_bytes / secs) / (1024.0 * 1024.0));
    close(fd);
    return 0;
}
