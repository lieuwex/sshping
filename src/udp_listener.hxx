#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

class UdpListener {
  public:
	UdpListener(int64_t rate) { arbiter_bandwidth.store(rate); }
	~UdpListener() { stop(); }

	int64_t get_bandwidth() {
		return arbiter_bandwidth.load(std::memory_order_relaxed);
	}

	void start() {
		udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
		if (udp_fd == -1) {
			throw std::runtime_error(
				"failed to create UDP socket: " + std::string(strerror(errno)));
		}

		struct sockaddr_in router_addr = {0};
		router_addr.sin_family = AF_INET;
		router_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		router_addr.sin_port = htons(7070);
		if (inet_pton(AF_INET, "127.0.0.1", &router_addr.sin_addr) != 1) {
			close(udp_fd);
			throw std::runtime_error("failed to parse IP");
		}

		if (connect(
				udp_fd, (struct sockaddr *)&router_addr, sizeof(router_addr))
			== -1) {
			close(udp_fd);
			throw std::runtime_error(
				"failed to connect to arbiter: "
				+ std::string(strerror(errno)));
		}

		shutdown_event_fd = eventfd(0, EFD_CLOEXEC);

		running = true;
		thread = std::thread(&UdpListener::loop, this);
	}

	void stop() {
		if (!running) {
			if (thread.joinable()) {
				thread.join();
			}
			return;
		}

		uint64_t signal_val = 1;
		if (write(shutdown_event_fd, &signal_val, sizeof(signal_val)) == -1) {
			fprintf(
				stderr, "UDP Listener: Error writing to shutdown eventfd: %s\n",
				strerror(errno));
		}

		thread.join();
		running = false;
	}

  private:
	int udp_fd = -1;
	int shutdown_event_fd = -1;
	std::thread thread;
	std::atomic<bool> running {false};

	std::atomic<int64_t> arbiter_bandwidth;

	const uint8_t PONG[1] = {3};
	const uint8_t RELEASE[1] = {2};

	struct Request {
		int64_t min_bandwidth;
		int64_t max_bandwidth;
		uint8_t priority;
	};

	class BufferWriter {
	  public:
		template <typename T> void write(T val) {
			T net;
			switch (sizeof(val)) {
			case 1: net = val; break;
			case 2: net = __builtin_bswap16(val); break;
			case 4: net = __builtin_bswap32(val); break;
			case 8: net = __builtin_bswap64(val); break;
			}

			const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&net);
			buffer.insert(buffer.end(), bytes, bytes + sizeof(net));
		}

		const std::vector<uint8_t> &data() const { return buffer; }

	  private:
		std::vector<uint8_t> buffer;
	};

	int8_t read_int8_be(unsigned char **cur, size_t *sz) {
		int8_t res = **cur;
		(*cur)++;
		(*sz)--;
		return res;
	}
	int64_t read_int64_be(unsigned char **cur, size_t *sz) {
		assert((*sz) >= 8);

		unsigned char *b = *cur;
		(*cur) += 8;
		(*sz) -= 8;

		return int64_t(b[0]) << 56 | int64_t(b[1]) << 48 | int64_t(b[2]) << 40
			   | int64_t(b[3]) << 32 | int64_t(b[4]) << 24 | int64_t(b[5]) << 16
			   | int64_t(b[6]) << 8 | int64_t(b[7]);
	}

	void handleMessage() {
		const size_t BUF_SIZE = 4096;
		unsigned char buf[BUF_SIZE] = {0};

		auto n = recv(udp_fd, buf, BUF_SIZE, 0);
		if (n == -1) {
			fprintf(stderr, "recv error: %s\n", strerror(errno));
			return;
		}

		switch (buf[0]) {
		// PING
		case 1: send(udp_fd, PONG, 1, 0); break;

		// allocated bandwidth
		case 2: {
			unsigned char *cur = buf + 1;
			size_t sz = n - 1;

			int64_t bandwidth = read_int64_be(&cur, &sz);
			arbiter_bandwidth.store(bandwidth, std::memory_order_relaxed);

			uint8_t _tin = read_int8_be(&cur, &sz);

			int64_t ts = read_int64_be(&cur, &sz);
			auto now = std::chrono::duration_cast<std::chrono::microseconds>(
						   std::chrono::system_clock::now().time_since_epoch())
						   .count();

			fprintf(stderr, "bandwidth = %ld at %ld µs\n", bandwidth, now - ts);
			break;
		}

		// FULL
		case 3:
			printf("NETWORK IS FULL\n");
			exit(1);
			break;

		default: printf("unknown message type: %d\n", buf[0]); break;
		}
	}

	void sendRequest(struct Request req) {
		BufferWriter w;
		w.write<uint8_t>(1);
		w.write<uint64_t>(getpid());
		w.write<int64_t>(req.min_bandwidth);
		w.write<int64_t>(req.max_bandwidth);
		w.write<uint8_t>(req.priority);
		send(udp_fd, w.data().data(), w.data().size(), 0);
	}

	void loop() {
		std::array<pollfd, 2> fds = {
			pollfd {
				.fd = udp_fd,
				.events = POLLIN,
			},
			pollfd {
				.fd = shutdown_event_fd,
				.events = POLLIN,
			}};

		sendRequest({
			.min_bandwidth = 1,
			.max_bandwidth = get_bandwidth(),
			.priority = 1,
		});

		while (running) {
			if (poll(fds.data(), fds.size(), -1) == -1) {
				running = false;
				break;
			}

			if (fds[0].revents & POLLERR) {
				// could not connect to arbiter
				fprintf(stderr, "arbiter is offline\n");
				running = false;
				break;
			} else if (fds[0].revents & POLLIN) {
				handleMessage();
			}

			if (fds[1].revents & POLLIN) {
				running = false;
				break;
			}
		}

		send(udp_fd, RELEASE, 1, 0);

		if (shutdown_event_fd != -1) {
			close(shutdown_event_fd);
			shutdown_event_fd = -1;
		}

		if (udp_fd != -1) {
			close(udp_fd);
			udp_fd = -1;
		}
	}
};
