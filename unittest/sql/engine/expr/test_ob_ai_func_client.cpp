/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"

using namespace oceanbase;
using namespace oceanbase::common;

// Quarantine released blocks, poisoning their bytes.  Unlike an arena, this
// makes a reference to a destroyed serialization buffer fail deterministically.
class PoisonAllocator : public ObIAllocator
{
public:
  ~PoisonAllocator() override
  {
    for (auto &block : blocks_) { std::free(block.first); }
  }
  void *alloc(const int64_t size) override
  {
    void *ptr = std::malloc(size);
    if (ptr != nullptr) { blocks_.emplace(ptr, size); }
    return ptr;
  }
  void *alloc(const int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *ptr) override
  {
    auto it = blocks_.find(ptr);
    if (it != blocks_.end()) { std::memset(ptr, 0xa5, it->second); }
  }
private:
  std::unordered_map<void *, size_t> blocks_;
};

class LoopbackJsonServer
{
public:
  explicit LoopbackJsonServer(const int count, const char *response = nullptr)
      : fd_(-1), count_(count), fixed_response_(response == nullptr ? "" : response),
        echo_(response == nullptr)
  {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_GE(fd_, 0);
    EXPECT_EQ(0, bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    socklen_t length = sizeof(address);
    EXPECT_EQ(0, getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length));
    port_ = ntohs(address.sin_port);
    EXPECT_EQ(0, listen(fd_, count));
    thread_ = std::thread([this] { serve(); });
  }
  ~LoopbackJsonServer()
  {
    shutdown(fd_, SHUT_RDWR);
    if (thread_.joinable()) { thread_.join(); }
    close(fd_);
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/echo"; }
  void join() { thread_.join(); }
  const std::vector<std::string> &bodies() const { return bodies_; }
private:
  void serve()
  {
    for (int i = 0; i < count_; ++i) {
      pollfd pending{fd_, POLLIN, 0};
      if (poll(&pending, 1, 10000) <= 0) { break; }
      int client = accept(fd_, nullptr, nullptr);
      if (client < 0) { break; }
      timeval timeout{5, 0};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      std::string request;
      size_t header_end = std::string::npos;
      size_t expected = 0;
      char buffer[4096];
      while (true) {
        ssize_t length = recv(client, buffer, sizeof(buffer), 0);
        if (length <= 0) { break; }
        request.append(buffer, length);
        if (header_end == std::string::npos) {
          size_t separator = request.find("\r\n\r\n");
          if (separator != std::string::npos) {
            header_end = separator + 4;
            size_t start = request.find("Content-Length:");
            if (start != std::string::npos) {
              expected = std::stoul(request.substr(start + 15));
            }
          }
        }
        if (header_end != std::string::npos && request.size() - header_end >= expected) { break; }
      }
      const std::string body = header_end == std::string::npos ? "" : request.substr(header_end, expected);
      bodies_.push_back(body);
      const std::string payload = echo_ ? body : fixed_response_;
      const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
          + std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
      size_t sent = 0;
      while (sent < response.size()) {
        ssize_t length = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (length <= 0) { break; }
        sent += length;
      }
      close(client);
    }
  }
  int fd_;
  int count_;
  uint16_t port_;
  std::string fixed_response_;
  bool echo_;
  std::thread thread_;
  std::vector<std::string> bodies_;
};

class AIFuncClientTest : public ::testing::Test
{
protected:
  ObJsonObject *request(const std::string &text)
  {
    ObJsonObject *object = OB_NEWx(ObJsonObject, &allocator_, &allocator_);
    ObString value;
    EXPECT_EQ(OB_SUCCESS, ob_write_string(allocator_, ObString(text.size(), text.data()), value));
    ObJsonString *string = OB_NEWx(ObJsonString, &allocator_, value);
    EXPECT_EQ(OB_SUCCESS, object->add("text", string));
    return object;
  }
  void check(ObJsonObject *response, const std::string &expected)
  {
    ASSERT_NE(nullptr, response);
    ObIJsonBase *value = response->get_value("text");
    ASSERT_NE(nullptr, value);
    ASSERT_EQ(ObJsonNodeType::J_STRING, value->json_type());
    EXPECT_EQ(expected, std::string(value->get_data(), value->get_data_length()));
  }
  PoisonAllocator allocator_;
};

TEST_F(AIFuncClientTest, SingleRequestOwnsSerializedBody)
{
  LoopbackJsonServer server(1);
  std::string url = server.url();
  std::string value(65536, 'x');
  value += "\n你好\"\\";
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  ObAIFuncClient client;
  ObJsonObject *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, client.send_post(allocator_, ObString(url.c_str()), headers, request(value), response));
  check(response, value);
  server.join();
  ASSERT_EQ(1, server.bodies().size());
}

TEST_F(AIFuncClientTest, BatchRequestsOwnDistinctSerializedBodies)
{
  LoopbackJsonServer server(3);
  std::string url = server.url();
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  std::vector<std::string> values{std::string(8192, 'a'), "second\n你好", std::string(32768, 'c')};
  ObArray<ObJsonObject *> requests;
  ObArray<ObJsonObject *> responses;
  for (const auto &value : values) { ASSERT_EQ(OB_SUCCESS, requests.push_back(request(value))); }
  ObAIFuncClient client;
  ASSERT_EQ(OB_SUCCESS, client.send_post_batch(allocator_, ObString(url.c_str()), headers, requests, responses));
  ASSERT_EQ(values.size(), responses.count());
  for (int i = 0; i < responses.count(); ++i) { check(responses.at(i), values[i]); }
  server.join();
  ASSERT_EQ(values.size(), server.bodies().size());
}

TEST_F(AIFuncClientTest, RejectsNonObjectResponseRoots)
{
  for (const char *body : {"[]", "1", "null", "\"text\""}) {
    LoopbackJsonServer server(1, body);
    std::string url = server.url();
    ObArray<ObString> headers;
    ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
    ObAIFuncClient client;
    ObJsonObject *response = nullptr;
    EXPECT_EQ(OB_ERR_INVALID_JSON_TEXT,
        client.send_post(allocator_, ObString(url.c_str()), headers, request("x"), response));
    EXPECT_EQ(nullptr, response);
    server.join();
  }
}

TEST_F(AIFuncClientTest, RejectsNonObjectBatchResponseRoots)
{
  LoopbackJsonServer server(1, "[]");
  std::string url = server.url();
  ObArray<ObString> headers;
  ObArray<ObJsonObject *> requests;
  ObArray<ObJsonObject *> responses;
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  ASSERT_EQ(OB_SUCCESS, requests.push_back(request("x")));
  ObAIFuncClient client;
  EXPECT_EQ(OB_ERR_INVALID_JSON_TEXT,
      client.send_post_batch(allocator_, ObString(url.c_str()), headers, requests, responses));
  EXPECT_EQ(0, responses.count());
  server.join();
}
