#pragma once

#include <iostream>
#include <thread>

#include <grpcpp/grpcpp.h>

#include <userver/engine/task/task.hpp>
#include <userver/utest/utest.hpp>

#include <userver/clients/grpc/manager.hpp>
#include <userver/clients/grpc/service.hpp>

namespace clients::grpc::test {

template <typename GrpcService, typename GrpcServiceImpl>
class GrpcServiceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("localhost:0", ::grpc::InsecureServerCredentials(),
                             &server_port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    std::cout << "Test fixture GRPC server started on port " << server_port_
              << "\n";
    server_thread_ = std::make_unique<std::thread>([&]() { server_->Wait(); });
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
    if (server_thread_ && server_thread_->joinable()) {
      server_thread_->join();
    }
  }

  auto ClientChannel() { return manager_.GetChannel(ServerEndpoint()); }

  auto ClientStub() const { return GrpcService::NewStub(ClientChannel()); }

  auto& GetQueue() { return manager_.GetCompletionQueue(); }

 private:
  std::string ServerEndpoint() const {
    return "localhost:" + std::to_string(server_port_);
  }

 private:
  int server_port_ = 0;
  GrpcServiceImpl service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<std::thread> server_thread_;
  Manager manager_{engine::current_task::GetTaskProcessor()};
};

}  // namespace clients::grpc::test
