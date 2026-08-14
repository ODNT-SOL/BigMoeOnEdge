// Standalone TCP server for the F3 remote expert-cache prototype (Phase 3a).
//
// Serves raw byte ranges from a model file. The eventual integration will speak
// ExpertKey requests and look up GGUF offsets; this version proves the RoCE link
// and measures throughput.
//
// Usage:
//   remote_cache_server <file> <listen_addr:port>
//
// Protocol:
//   request  : [u64 offset][u64 size]  (16 bytes)
//   response : [u64 size][bytes...]   (size may be 0 for EOF/error)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

static bool parse_addr(const char * s, std::string & host, int & port) {
    const char * colon = std::strrchr(s, ':');
    if (!colon || colon == s) return false;
    host.assign(s, colon - s);
    port = std::atoi(colon + 1);
    return port > 0 && port <= 65535;
}

static int listen_socket(const char * host, int port) {
    struct addrinfo hints{}, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char port_s[16];
    std::snprintf(port_s, sizeof(port_s), "%d", port);
    if (getaddrinfo(host, port_s, &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto * p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 && listen(fd, 16) == 0) break;
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

static void handle_client(int cfd, int file_fd, off_t file_size) {
    std::vector<uint8_t> buf(1 << 20);
    while (true) {
        uint8_t req[16];
        if (!recv_fully(cfd, req, 16)) break;
        uint64_t offset = decode_u64(req);
        uint64_t size   = decode_u64(req + 8);
        if (offset > uint64_t(file_size)) {
            uint8_t rsp[8] = {};
            send_fully(cfd, rsp, 8);
            break;
        }
        size_t to_send = size_t(std::min<uint64_t>(size, uint64_t(file_size) - offset));
        uint8_t hdr[8];
        encode_u64(hdr, to_send);
        if (!send_fully(cfd, hdr, 8)) break;
        while (to_send) {
            size_t chunk = std::min<size_t>(to_send, buf.size());
            ssize_t n = pread(file_fd, buf.data(), chunk, off_t(offset));
            if (n <= 0) break;
            if (!send_fully(cfd, buf.data(), size_t(n))) break;
            to_send -= n;
            offset += n;
        }
    }
    close(cfd);
}

int main(int argc, char ** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <file> <listen_addr:port>\n", argv[0]);
        return 1;
    }
    const char * path = argv[1];
    std::string host;
    int port = 0;
    if (!parse_addr(argv[2], host, port)) {
        std::fprintf(stderr, "Bad address: %s\n", argv[2]);
        return 1;
    }
    int file_fd = open(path, O_RDONLY | O_DIRECT);
    if (file_fd < 0) file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        std::perror("open");
        return 1;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        std::perror("fstat");
        return 1;
    }
    int lfd = listen_socket(host.c_str(), port);
    if (lfd < 0) {
        std::perror("listen");
        return 1;
    }
    std::printf("Serving %s (%ld bytes) on %s:%d\n", path, long(st.st_size), host.c_str(), port);
    while (true) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int cfd = accept(lfd, reinterpret_cast<struct sockaddr *>(&addr), &addrlen);
        if (cfd < 0) continue;
        std::thread(handle_client, cfd, file_fd, st.st_size).detach();
    }
}
