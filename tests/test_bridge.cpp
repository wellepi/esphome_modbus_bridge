#include "host_runtime.h"
#include "../components/modbus_bridge/modbus_bridge.cpp"

using namespace esphome;
using namespace esphome::modbus_bridge;

class TestBridge : public ModbusBridgeComponent {
public:
  using ModbusBridgeComponent::check_tcp_sockets_;
  using ModbusBridgeComponent::poll_uart_response_;
  using ModbusBridgeComponent::poll_trusted_host_lookup_;
  size_t queued() const { return pending_requests_.size(); }
  uint16_t active_tid() const { return transaction_id_from_header_(pending_requests_.front().header); }
  bool running() const { return sock_ >= 0; }
  bool resolving() const { return trusted_hosts_resolving_; }
  bool pending(size_t slot = 0) const { return clients_[slot].trust_pending; }
  bool trusted(size_t slot = 0) const { return clients_[slot].trusted; }
  size_t buffered(size_t slot = 0) const {
#ifdef USE_ESP32
    return rx_accu_[slot].size();
#else
    return rx_accu8266_[slot].size();
#endif
  }
};

struct Fixture {
  uart::UARTComponent uart;
  TestBridge bridge;
  Fixture() {
    fake::now = 0;
    fake::connections.clear(); fake::accepts.clear(); fake::next_fd = 3;
    fake::dns_requests.clear(); fake::dns_tasks.clear(); fake::dns_cached.clear();
    fake::dns_error = false; fake::dns_queue_full = false; fake::logs.clear();
    network::connected = true;
    network::addresses = {}; network::addresses[0].value = 0xC0A80164;
    bridge.set_uart_id(&uart); bridge.setup();
    network_tick();
  }
  void network_tick() { bridge.run_interval("tcp_server_and_network_check"); }
  void poll() { bridge.check_tcp_sockets_(); }
  int connect(uint32_t ip = 0xC0A80120) { int fd = fake::client(ip); poll(); return fd; }
  void incoming(int fd, const std::vector<uint8_t> &data) {
    auto &rx = fake::connections[fd].rx; rx.insert(rx.end(), data.begin(), data.end());
  }
  void receive(const std::vector<uint8_t> &data, uint32_t at) {
    fake::now = at; uart.rx.insert(uart.rx.end(), data.begin(), data.end()); bridge.poll_uart_response_();
  }
  void settle(uint32_t at) {
    fake::now = at; bridge.poll_uart_response_(); fake::now = at + 7; bridge.poll_uart_response_();
  }
  void resolve(uint32_t ip) {
    fake::run_dns_task(); fake::dns_reply(ip); poll();
  }
};

static std::vector<uint8_t> request(uint16_t tid, uint8_t fc = 4) {
  return {uint8_t(tid >> 8), uint8_t(tid), 0, 0, 0, 6, 1, fc, 0, 18, 0, 1};
}

static std::vector<uint8_t> with_crc(std::vector<uint8_t> data) {
  const uint16_t crc = modbus_crc(data.data(), data.size());
  data.push_back(uint8_t(crc)); data.push_back(uint8_t(crc >> 8)); return data;
}

static void test_tcp_buffer() {
  Fixture f; int fd = f.connect();
  for (uint16_t tid = 1; tid <= 10; ++tid) f.incoming(fd, request(tid));
  f.poll();
  assert(f.bridge.queued() == 8 && f.bridge.buffered() == 24);
  assert(fake::connections[fd].rx.empty());
  f.poll();
  assert(f.bridge.queued() == 10 && f.bridge.buffered() == 0);
  assert(f.uart.tx.size() == 1);
}

static void test_tcp_fragments() {
  Fixture f; int fd = f.connect(); auto data = request(1);
  f.incoming(fd, {data.begin(), data.begin() + 5}); f.poll();
  assert(f.bridge.queued() == 0 && f.bridge.buffered() == 5);
  f.poll(); assert(f.bridge.queued() == 0);
  f.incoming(fd, {data.begin() + 5, data.end()}); f.poll();
  assert(f.bridge.queued() == 1 && f.uart.tx.size() == 1);
}

static void test_echo_and_delayed_response() {
  Fixture f; int fd = f.connect();
  f.incoming(fd, request(1)); f.incoming(fd, request(2)); f.poll();
  const auto echo = f.uart.tx.front();
  f.receive(echo, 7); f.settle(14);
  assert(f.uart.tx.size() == 1 && f.bridge.active_tid() == 1);
  f.receive({1,4,2,0,0xA7,0xF8,0x8A}, 50);
  assert(f.uart.tx.size() == 2 && f.bridge.active_tid() == 2);
  const auto &reply = fake::connections[fd].tx;
  assert((reply == std::vector<uint8_t>{0,1,0,0,0,5,1,4,2,0,0xA7}));
}

static void test_invalid_responses_keep_deadline() {
  Fixture f; int fd = f.connect(); f.incoming(fd, request(1)); f.incoming(fd, request(2)); f.poll();
  f.receive(with_crc({2,4,2,0,0}), 7); f.settle(14); // other UID
  assert(f.uart.tx.size() == 1);
  f.receive({1,4,2,0,0,0,0}, 100); f.settle(107); // bad CRC
  assert(f.uart.tx.size() == 1);
  f.receive({0x55}, 900); f.settle(907); // noise
  assert(f.uart.tx.size() == 1);
  fake::now = 1001; f.bridge.poll_uart_response_();
  assert(f.uart.tx.size() == 2 && f.bridge.active_tid() == 2);
  assert(std::any_of(fake::logs.begin(), fake::logs.end(), [](const auto &s) {
    return s.find("no valid matching response") != std::string::npos;
  }));
}

static void test_valid_write_and_exception() {
  for (uint8_t fc : {uint8_t(6), uint8_t(4)}) {
    Fixture f; int fd = f.connect(); f.incoming(fd, request(1, fc)); f.poll();
    f.receive(fc == 6 ? f.uart.tx.front() : with_crc({1,0x84,2}), 7);
    assert(f.bridge.queued() == 0 && !fake::connections[fd].tx.empty());
  }
}

static void test_no_response_and_incomplete() {
  for (bool incomplete : {false, true}) {
    Fixture f; int fd = f.connect(); f.incoming(fd, request(1)); f.poll();
    if (incomplete) f.receive({1,4,2,0}, 7);
    fake::now = 1001; f.bridge.poll_uart_response_();
    assert(f.bridge.queued() == 0 && fake::connections[fd].tx.empty());
  }
}

static void test_network_recovery() {
  Fixture f; int fd = f.connect(); f.incoming(fd, request(1)); f.poll();
  network::addresses = {}; f.network_tick();
  assert(!f.bridge.running() && f.bridge.queued() == 0);
  assert(!fake::connections[fd].connected);
  network::addresses[0] = {1, false}; f.network_tick(); assert(!f.bridge.running());
  network::addresses[0] = {0xC0A80164, true}; network::connected = false;
  f.network_tick(); assert(!f.bridge.running());
  network::connected = true; f.network_tick(); assert(f.bridge.running());
  int next = f.connect(); f.incoming(next, request(2)); f.poll(); assert(f.bridge.active_tid() == 2);
}

static void test_dns_pending_and_existing_client() {
  Fixture f;
  f.bridge.add_trusted_network(0xC0A80100, 0xFFFFFF00);
  f.bridge.add_trusted_host("home.example"); f.bridge.set_reject_untrusted_clients(true);
  int local = f.connect(); assert(!f.bridge.resolving());
  int remote = f.connect(0xCB007109); assert(f.bridge.pending(1));
  f.incoming(remote, request(2)); f.incoming(local, request(1)); f.poll();
  assert(f.uart.tx.size() == 1 && f.bridge.active_tid() == 1);
  f.receive(with_crc({1,4,2,0,42}), 20); assert(f.bridge.queued() == 0);
  f.resolve(0xCB007109);
  assert(!f.bridge.pending(1) && f.bridge.trusted(1));
  f.poll(); assert(f.bridge.active_tid() == 2);
}

static void test_dns_failure_and_cache() {
  Fixture f; f.bridge.add_trusted_host("home.example"); f.bridge.set_reject_untrusted_clients(true);
  int remote = f.connect(0xCB007109); f.incoming(remote, request(1));
  f.resolve(0);
  assert(!fake::connections[remote].connected && f.uart.tx.empty());
  int again = f.connect(0xCB007109);
  assert(!fake::connections[again].connected && !f.bridge.resolving());
  fake::now = 60001; remote = f.connect(0xCB007109); assert(f.bridge.resolving());
  f.resolve(0xCB007109); assert(f.bridge.trusted());
}

static void test_dns_shutdown_and_reuse() {
  Fixture f; f.bridge.add_trusted_host("home.example"); f.bridge.set_reject_untrusted_clients(true);
  int old = f.connect(0xCB007109); fake::run_dns_task();
  network::connected = false; f.network_tick(); assert(!fake::connections[old].connected);
  network::connected = true; f.network_tick(); int next = f.connect(0xCB007109);
  f.incoming(next, request(1)); f.resolve(0xCB007109); // obsolete result
  assert(f.bridge.pending() && f.uart.tx.empty());
  f.resolve(0); // current lookup failed; obsolete success must not grant access
  assert(!fake::connections[next].connected && f.uart.tx.empty());
}

static void test_dns_multiple_and_immediate() {
  Fixture f; f.bridge.add_trusted_host("first.example"); f.bridge.add_trusted_host("second.example");
  fake::dns_cached["first.example"] = 0xCB007108;
  int remote = f.connect(0xCB007109); fake::run_dns_task(); f.poll();
  assert(f.bridge.pending());
  f.resolve(0xCB007109);
  assert(!f.bridge.pending() && f.bridge.trusted());
  f.incoming(remote, request(1)); f.poll(); assert(f.uart.tx.size() == 1);
}

int main() {
  test_tcp_buffer(); test_tcp_fragments(); test_echo_and_delayed_response();
  test_invalid_responses_keep_deadline(); test_valid_write_and_exception(); test_no_response_and_incomplete();
  test_network_recovery(); test_dns_pending_and_existing_client(); test_dns_failure_and_cache();
  test_dns_shutdown_and_reuse(); test_dns_multiple_and_immediate();
#ifdef USE_ESP32
  puts("ESP32: 11 bridge regression tests passed");
#else
  puts("ESP8266: 11 bridge regression tests passed");
#endif
}
